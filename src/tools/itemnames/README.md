# Item Names Extractor

**Related Guides**: [Root Overview](../../../README.md) | [Game-Data Extractor](../aoc-extractor/README.md) | [Content Compiler](../../server/services/cmd/contentc/README.md)

`src/tools/itemnames` is an extraction utility that resolves localized display names, item categories, and equipment slot tags from the client string tables (`locres`) and DesignData records to generate `dist/content/items.tsv`.

---

## How `items.tsv` Is Used

- **`worldctl give-item`**: Lets operators grant items by human-readable name (such as `" Copper Ore "` or `" Novice Sword "`) or GUID.
- **Web Admin Catalog**: Powers item search and inventory display in `aoc-gateway` (`internal/admin-catalog`) under either `AOC_DATA_MODE_ITEMS=memory` or `disk`.

---

## Running the Tool

`itemnames` is invoked automatically during game-data extraction:

```powershell
.\sandal.ps1 extract
```
