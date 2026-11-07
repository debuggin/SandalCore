# Character Import CLI

**Related Guides**: [Root Overview](../../../../../README.md) | [Character Service](../character/README.md) | [Services Overview](../../README.md)

`src/server/services/cmd/charimport` (`charimport`) is a command-line utility that validates and imports character snapshots from JSON files (`characters.json`) into PostgreSQL (`aoc-postgres`).

---

## Usage

```powershell
# Validate a characters.json file without writing to the database
docker exec aoc-character charimport --file /app/content/characters.json --dry-run

# Import character records idempotently into PostgreSQL
docker exec aoc-character charimport --file /app/content/characters.json
```

---

## Flags

| Flag | Default | Description |
|:---|:---|:---|
| `--file` | `characters.json` | Path to the JSON character snapshot file to validate or import. |
| `--db` | `$DATABASE_URL` | PostgreSQL connection URL. |
| `--dry-run` | `false` | Validates JSON schema and prints a summary without committing changes. |
