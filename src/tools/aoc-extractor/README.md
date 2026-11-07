# Game-Data Extractor

**Related Guides**: [Root Overview](../../../README.md) | [Configuration Reference](../../../docs/CONFIGURATION.md) | [Content Compiler](../../server/services/cmd/contentc/README.md) | [Map Compiler](../mapc/README.md)

`src/tools/aoc-extractor` (`AoCExtractor.csproj`) is the game-data extraction tool for SandalCore. Powered by `CUE4Parse`, it reads your installed Ashes of Creation client archives (`AOC/Content/Paks/*.utoc` and `*.ucas`) and extracts the tables, map tiles, and geometry required to compile `dist/content/content.db` (schema `v41`) and the 25 runtime game-data files.

---

## What It Extracts

When `.\start.bat` or `.\sandal.ps1 extract` runs, `AoCExtractor` produces the raw and compiled assets in `dist/content/`:

- **DesignData Records**: Extracts item definitions, NPC templates, abilities, quests, crafting recipes, skill trees, and settlement layouts for compilation into `dist/content/content.db` (`v41`).
- **Map Tiles and Icons**: Extracts the `1,713` valid `TUI_s50000` minimap tiles (`verra.pmtiles` up to `z=7`) and settlement/POI map icons (`dist/content/icons/`).
- **World Geometry and Foliage**: Extracts terrain heightfields, town building footprints, road spline tables (`roads.tsv`, `roads_levels.tsv`), Zone of Influence polygons (`zoi_polygons.tsv`), City Node OBB bounds (`node_plug_bounds.tsv`), and gatherable foliage spawners (`region_foliage.tsv`).

---

## Local `.usmap` Override

By default, `AoCExtractor` uses the checked-in mappings file at `src/config/layouts/aoc.usmap`. If you place a newer `.usmap` file at `$HOME/.aoc/aoc.usmap` (or set `SANDAL_USMAP_PATH`), `scripts/lib/Env.psm1` and `AoCExtractor` automatically copy it to `dist/content/aoc.usmap` and normalize `.usmap` v4 headers in memory.

---

## Running the Extractor

```powershell
# Run the full extraction and content.db compilation pipeline
.\sandal.ps1 extract

# Force a fast content.db recompile without re-extracting unchanged client paks
pwsh -NoProfile -File scripts/extract_game_data.ps1 -ForceContentDb
```
