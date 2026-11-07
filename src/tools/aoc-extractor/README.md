# SandalCore Game-Data Extraction Pipeline (`src/tools/aoc-extractor`)

**Primary Diataxis Quadrant**: `Technical Reference` (`DOC-01`)
**Related Guides**: [Root Architecture Overview](../../../README.md) | [Configuration Reference](../../../docs/CONFIGURATION.md) | [Server Workers & Spatial Authority](../../../docs/SERVER_WORKERS.md) | [`libaocnav` Navigation Reference](../../libs/aocnav/README.md) | [`mapc` Map Compiler Reference](../mapc/README.md)

`src/tools/aoc-extractor` (`AocExtractor.csproj`) and its companion compilers (`contentc`, `mapc`, `aoc_navbuild`, `pmtiles-gen`, `itemnames`, `npcabilities`, and `schemagen`) form the clean-room game-data extraction pipeline. Invoked via `.\sandal.ps1 extract` (or automatically on startup when `dist/content/content.db` schema `v41` or any of the 25 mandatory runtime artifacts is missing), this pipeline parses UE5.6 IoStore containers (`.utoc` / `.ucas` / `.pak`) and `.usmap` reflection mappings via `CUE4Parse` and compiles them into `dist/content/`.

---

## 1. Extractor Subcommands (`src/tools/aoc-extractor/Program.cs`)

`aoc-extractor` is a `.NET 10` (`net10.0`) CLI tool supporting the following subcommands:

| Subcommand | Output Artifacts | Description |
|:---|:---|:---|
| **`extract-all`** | Raw JSON/TSV dumps -> `contentc` & `mapc` inputs | Runs the unified single-pass IoStore extraction pipeline across DesignData, world terrain heightfields, collision meshes, and map textures. |
| **`extract-db`** | DesignData JSON records for `contentc` | Extracts all `DesignDataObject` records (`NPC`, `Item`, `Ability`, `Effect`, `Quest`, `CityNode`, `SkillTree`, `Recipe`, `LootTable`, `Spawner`) from `Verra_01` packages. |
| **`extract-vmap`** | Raw landscape heightfields & static mesh collision | Extracts 16-bit landscape heightmap tiles (`Verra_01` world partition) and cooked Chaos/PhysX static mesh geometries for `mapc` and `aoc_navbuild`. |
| **`extract-nav`** | Recast/Detour navmesh input geometry | Extracts walkable slope, water level, and settlement platform geometry for tiled Detour navigation mesh generation. |
| **`extract-tiles`** | Orthographic WebP/PNG map tiles | Extracts UI minimap and world map textures stitched by `pmtiles-gen` into `dist/content/verra.pmtiles`. |
| **`verify-standin`** | Exit code `0` / diagnostic report | Verifies extracted asset completeness and schema parity against the expected `content.db` contract. |

### Automatic `.usmap` v4 -> v3 Downgrade (`UsmapCompat.cs`)

UE5.6 ships unversioned property mappings using `.usmap` format v4 (`EMapVersion = 4`). `UsmapCompat.cs` automatically inspects `$HOME/.aoc/usmap/*` (`aoc.usmap`) or `dist/content/aoc.usmap` and performs an in-memory v4-to-v3 header normalization when required by `CUE4Parse`.

---

## 2. Companion Compilation Tools (`src/tools/*` & `src/server/services/cmd/*`)

After `aoc-extractor` reads the raw UE5 packages, `scripts/sandal/extract.ps1` orchestrates the following specialized compilers:

1. **`contentc` (`src/server/services/cmd/contentc`)**:
   - Compiles extracted DesignData into the SQLite **`dist/content/content.db`** database (**Schema Version `v41`**, verified by `Ensure-SandalContentDbCurrent`), builds all **13 covering and partial SQLite indexes** (`record_by_guid`, `record_by_name`, `record_by_type_guid`, `idx_spawners_location`, etc.), runs `ANALYZE`, and exports `roads.bin` (`contentc roads --levels`).
2. **`mapc` (`src/tools/mapc`)**:
   - Compiles raw terrain heightfields, biome masks, settlement platforms, and Voronoi Zone of Influence definitions into binary runtime structures (`area_geom.bin`, `area_nav.bin`, `town_platforms.bin`, `town_layouts.bin`, `water_bodies.bin`, `foliage.bin`, `world_bounds.json`) and **tab-separated `.tsv` tables**.
3. **`aoc_navbuild` (`src/tools/navbuild`)**:
   - Multi-threaded C++23 Recast/Detour v1.6.0 offline compiler linked against `libaocnav` that builds tiled 3D navigation meshes (`area_nav.bin` / `nav_chunks/*.bin`) with detail-edge height repair (`TileRepair`) and elevated city platform stitching (`TownPlatforms`).
4. **`pmtiles-gen` (`src/tools/pmtiles-gen`)**:
   - Go utility that packs extracted orthographic map tiles into a single-file PMTiles v3 archive (`dist/content/verra.pmtiles`, zoom levels `z0..z7`) served by `aoc-web-admin` and `aoc-gateway`.

---

## 3. The 25 Mandatory Runtime Game-Data Artifacts & `.tsv` Storage Modes

`Test-SandalRuntimeGameDataComplete` (`scripts/lib/Env.psm1`) verifies that `dist/content/` and `deploy/content/` contain all **25 mandatory runtime artifacts**, including:

- **Core Databases & Map Archives**: `content.db` (schema `v41`), `verra.pmtiles`, `world_bounds.json`, `aoc.usmap`.
- **Spatial & Navigation Binaries**: `area_geom.bin`, `area_nav.bin`, `town_platforms.bin`, `town_layouts.bin`, `roads.bin`, `water_bodies.bin`, `foliage.bin`.
- **Authoritative `.tsv` Catalog & Spatial Tables**: `zoi_polygons.tsv` (27 Settlement & Divine Gate ZOIs), `node_plug_bounds.tsv` (`1 km x 1 km` rotated CityNode OBBs), `city_nodes.tsv` (85 settlements), `pois.tsv`, `npcs.tsv`, `items.tsv`, `spawners.tsv`, `abilities.tsv`, `quests.tsv`, `recipes.tsv`, `loot_tables.tsv`, `skill_trees.tsv`, `starting_gear.tsv`, and `biome_regions.tsv`.

### Runtime `.tsv` Storage Modes (`memory` vs. `disk`)

Both C++23 `world` workers and Go control-plane services (`admin-catalog`) consume the extracted `.tsv` files in `deploy/content/*.tsv` according to the per-data-type storage mode configured in root `.env`:

- **`AOC_DATA_MODE_DEFAULT=memory|disk`** (default `memory`)
- **Per-Data-Type Overrides**: `AOC_DATA_MODE_ZOIS`, `AOC_DATA_MODE_CITY_NODES`, `AOC_DATA_MODE_POIS`, `AOC_DATA_MODE_NPCS`, `AOC_DATA_MODE_ITEMS`.
  - When set to `memory`, the `.tsv` table is parsed once at process startup into an immutable in-memory index for $O(1)$ zero-I/O lookups.
  - When set to `disk`, the service maintains a compact byte-offset index and streams records on demand from `deploy/content/*.tsv` to minimize container RSS memory.

> [!IMPORTANT]
> Never stage or commit extracted game-data files (`content.db`, `*.bin`, `*.pmtiles`, or extracted `.tsv` files outside `cmd/contentc/` unit-test fixtures) to Git. `scripts/checks/public_check.ps1` enforces zero checked-in extractor outputs.
