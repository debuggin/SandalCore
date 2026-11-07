# Content Compiler

**Related Guides**: [Root Overview](../../../../../README.md) | [Game-Data Extractor](../../../../tools/aoc-extractor/README.md) | [Stat Bytecode Engine](../../internal/statprog/README.md) | [World Server](../../../world/README.md)

`src/server/services/cmd/contentc` (`contentc`) compiles extracted Ashes of Creation DesignData JSON records into the SQLite database `dist/content/content.db` (schema `v41`) and companion `.tsv` tables used by `world` and the control plane.

---

## What `contentc` Produces

1. **Schema `v41` `content.db`**: Populates the normalized SQLite tables for items, NPCs, abilities, status effects, quests, crafting recipes, skill trees, settlement populations, and `AOSP` stat bytecode programs (`internal/statprog`).
2. **13 Covering Indexes and `ANALYZE`**: Creates all 13 covering and partial SQLite indexes (`record_by_guid`, `records_by_type_id`, `edges_by_source`, `edges_by_target`, etc.) and runs `ANALYZE` so startup and runtime queries run without full-table scans.
3. **Road and Spatial Tables**: Emits `roads.tsv` and `roads_levels.tsv` used for road movement speed buffs and map overlays.

---

## Automatic Schema Ratchet

When `.\start.bat`, `.\build.bat`, or `.\sandal.ps1 {up,deploy,extract}` runs, `Ensure-SandalContentDbCurrent` (`scripts/lib/Env.psm1`) checks the `meta.schema_version` inside `dist/content/content.db`. If `content.db` is missing or its schema version does not match `kExpectedSchemaVersion` (`v41`), `contentc` recompiles `content.db` automatically.

---

## Running Manually

```powershell
# Recompile dist/content/content.db and verify all 25 runtime game-data files
.\sandal.ps1 extract
```
