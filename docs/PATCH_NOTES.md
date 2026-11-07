# SandalCore Patch Notes (`docs/PATCH_NOTES.md`)

**Primary Diataxis Quadrant**: `Technical Reference` (`DOC-01`)
**Related Guides**: [Root Architecture Overview](../README.md) | [Configuration Reference](CONFIGURATION.md) | [Server Workers & Spatial Authority](SERVER_WORKERS.md) | [Control-Plane Services Reference](SERVICES.md) | [Web Admin & GM Studio Reference](WEB_ADMIN.md) | [Roadmap](ROADMAP.md) | [Known Issues](KNOWN_ISSUES.md)

---

## 2026-11-07 (`v0.9.40` -- Multi-Tier Server Workers, Scoped JWT Web Admin, `.tsv` Data Modes & `content.db` `v41`)

### Server Workers, Spatial Authority & Multi-Boundary Handover ([`docs/SERVER_WORKERS.md`](SERVER_WORKERS.md))
- **6 Server Worker Topologies + Convex Polygon Cutouts + Mix-and-Match Composition**:
  - Supported authority topologies: (`1`) Whole-World (`aoc-world` at `10 Hz`), (`2`) Per-Region (`aoc-<region-name>` at `20 Hz`), (`3`) Per-ZOI (`aoc-<zoi-name>` at `30 Hz`), (`4`) Per-Node-Plug + Per-ZOI (`aoc-<city-name>` + `aoc-<zoi-name>` at `30 Hz`), (`5`) Dynamic Node-Plug + Dynamic ZOI Stub (`aoc-<city-name>-alpha..zulu` + `aoc-<zoi-name>-alpha..zulu`, `min = 1`, `max = 26` NATO alphabet per city/ZOI, currently a stub running `-alpha`), (`6`) Dynamic ZOI Stub (`aoc-<zoi-name>-alpha..zulu`, `min = 1`, `max = 26` NATO alphabet per ZOI, currently a stub running `-alpha`), (`7`) Additive Convex Polygon Cutout workers (`--area-polygon`), and (`8`) Mix-and-Match composition (such as Sandsquall running in `30 Hz` ZOI mode alongside `20 Hz` regional workers).
- **Mesh Worker Index (`worker_index`) Registration**: Every booting server worker registers with `aoc-mesh` (`:50055`) and receives a sequential `worker_index` (`1..N`) that seeds `ServerGuidAllocator(worker_index, randomizer)` for collision-free 128-bit `FGuid` allocation across shards.
- **Seamless Multi-Boundary Player Handover & Anti-Hysteresis**: Enforces a `5,000 cm` (`50 m`) inward border buffer, `3` consecutive dwell samples, a `2,000 ms` handover cooldown, and monotonic `lease_epoch` fencing across `region <-> region`, `region <-> zoi`, `zoi <-> zoi`, `zoi <-> node_plug`, and `polygon_cutout` boundaries.
- **Live CityNode Stage (`0..6`) Population Scaling & GM Actor Lifecycle**: Dynamic `NodePopulations` scaling with `reap_placed_stations` and `town_platforms.bin` (`AOCTPF01`) vertical $Z$-floor rescue, plus full GM actor lifecycle RPCs (`SpawnNpc`, `KillNpc`, `DespawnNpc`, `HealNpc`, `ResetNpc`, `RespawnNpc`, `MoveNpc`, `TeleportPlayer`).

### Configuration SSoT, Default Fallback, Tick Rates & `.tsv` Storage Modes ([`docs/CONFIGURATION.md`](CONFIGURATION.md))
- **Persistent Root `.env` SSoT & Single-Worker Riverlands Default Fallback**: Root `.env` persists across `build/`, `dist/`, and `deploy/` resets (`Sync-SandalDeployStage`). When root `.env` is absent, SandalCore defaults to `AOC_WORLD_SHARD_MODE=single` and `AOC_WORLD_REGION=riverlands`, launching **1 server worker (`aoc-riverlands` / `aoc-world-riverlands`, `11` containers total)** and spawning only Riverlands entities.
- **Configurable Tick Rates (`Hz`)**: `AOC_WORLD_TICK_HZ_ZOI=30` (`30 Hz`), `AOC_WORLD_TICK_HZ_REGION=20` (`20 Hz`), `AOC_WORLD_TICK_HZ_WORLD=10` (`10 Hz`), and `AOC_SERVICE_TICK_HZ=30` (`30 Hz` Go control-plane tick).
- **Per-Data-Type `.tsv` Storage Modes (`memory` vs. `disk`)**: Added `AOC_DATA_MODE_DEFAULT=memory|disk` and per-type overrides (`AOC_DATA_MODE_ZOIS`, `AOC_DATA_MODE_CITY_NODES`, `AOC_DATA_MODE_POIS`, `AOC_DATA_MODE_NPCS`, `AOC_DATA_MODE_ITEMS`) backed by `deploy/content/*.tsv`.
- **`content.db` Schema `v41` & 25 Mandatory Runtime Artifacts**: Automatic schema ratchet (`Ensure-SandalContentDbCurrent`), 13 SQLite covering indexes + `ANALYZE`, `$HOME/.aoc/certs/*` TLS certificate preservation, and `$HOME/.aoc/usmap/*` automatic v4 -> v3 compatibility normalization.

### Web Admin & GM Studio ([`docs/WEB_ADMIN.md`](WEB_ADMIN.md) & [`src/clients/web-admin/README.md`](../src/clients/web-admin/README.md))
- **Startup Passphrase -> Scoped `HS256` JWT Authentication**: Console startup banner prints `Username: admin` and ephemeral `Passphrase: <passphrase>`, exchanged via `POST /api/v1/admin/auth/login` for a scoped `HS256` JWT carrying `boot_id` (`AOC_ADMIN_BOOT_ID`) so browser tabs automatically re-prompt on server restart.
- **Viewport-Bounded Entity Streaming & Zoom-Tiered Update Hz (`5 Hz -> 30 Hz`)**: Server-side `minX, minY, maxX, maxY` viewport culling paired with non-linear zoom rate scaling (`z0: 5 Hz`, `z1: 6 Hz`, `z2: 7 Hz`, `z3: 9 Hz`, `z4: 12 Hz`, `z5: 16 Hz`, `z6: 22 Hz`, `z7: 30 Hz`).
- **27 Tile-Clipped Inward ZOI Borders, Mutually Exclusive Selection & Searchable GM Modals**: Renders all 27 Settlement and Divine Gate ZOIs with inward-offset border strokes, enforces mutually exclusive selection (`Entity` vs `Map Position` vs `CityNode` vs `POI`), and provides searchable `Teleport Player Here...` and `Spawn NPC / Monster Here...` modals plus the 4-zone Golden-Signals / Worker-Shards / USE-RED / Structured-Logs explorer.

---

## 2026-10-28 (`v0.9.35` -- 4-Tier Repository Layout, Native ICS `:50061` Character Creation & 10-Stage Live Play-Flow Gate)

- **4-Tier Directory Layout (`src/`, `build/`, `dist/`, `deploy/`)**: Unified all tracked source code under `src/`, out-of-tree build artifacts under `build/`, shippable binaries and extracted game data under `dist/`, and runtime state under `deploy/` with three root entrypoints (`start.bat`, `stop.bat`, `build.bat` -> `sandal.ps1`).
- **`.NET 10` `SandalLauncher` & Host `LocalTetherListener` (`127.0.0.1:19021/udp`)**: In-process UDP ARQ `AskWindow` / `TellWindow` (`0x04`) responder and native `:50061` ICS TLS 1.3 in-game character creation (`0` pre-launch stub characters).
- **100% Live Functional API & 10-Stage `fake::Client` Gate (`scripts/test_functional_apis.ps1`)**: End-to-end verification covering HTTP Auth (`:8080`), UDP Tether (`:19021`), ICS TLS 1.3 (`:50061`), and a 10-stage UDP `:7777` `fake::Client` session inside `aoc-world`.
