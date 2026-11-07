# Patch Notes

**Related Guides**: [Root Overview](../README.md) | [Configuration Reference](CONFIGURATION.md) | [Operational Runbook](RUNBOOK.md) | [Server Workers](SERVER_WORKERS.md) | [Web Admin Studio](WEB_ADMIN.md) | [Project Roadmap](ROADMAP.md) | [Known Issues](KNOWN_ISSUES.md)

This page records the release history and major changes in SandalCore.

---

## 2026-11-07 (`v0.9.42` -- Authentic Data Provenance, 4-Season Verra Calendar, Observability Tab & Scope-Tuned Worker Tick Rates)

### Server Workers and Spatial Authority ([`docs/SERVER_WORKERS.md`](SERVER_WORKERS.md))

- **Out-of-the-Box Single-Worker Riverlands Default and 5 Partition Topologies**:
  - Defaults to 1 regional worker for Riverlands (`AOC_WORLD_PARTITION_MODE=single_riverlands`, `45 Hz`, `11` containers total) and supports `single_world` (`30 Hz`), `per_region` (`45 Hz`), `per_zoi` (`60 Hz`), and `per_zoi_nodes_and_pois` (`60 Hz` ZOI + `100 Hz` City Node OBB and POI cut-out workers).
- **Per-Worker Runtime Tick-Rate Control and `0 Hz` NPC Pause**:
  - Operators can set any worker independently to `Dynamic (30-60 Hz)`, `0 Hz (Pause NPCs)`, or manual `10..60 Hz` from the Web Admin `Visible Server Workers` panel or `worldctl`, backed by a Little's Law `<= 1.0 s` (`>= 1 Hz`) entity service floor.
- **Unbounded Authentic Entity Populations and Client/PCAP Resource Provenance**:
  - Removed artificial `AOC_WORLD_MAX_*` and `max_gatherables` caps so entity counts are bounded only by in-memory container structures. Gatherable foliage and resource nodes come strictly from extracted client data (`region_foliage.tsv` and `content.db`) and retail packet captures (`pcap_resource_spawners.tsv`).
- **PostgreSQL Simulation Persistence and Tier 5/6 City Node Population Fallback**:
  - `mesh-gateway` persists global world time, calendar anchors, worker tick overrides (`world_simulation_state`), and settlement progression (`city_node_state`) in PostgreSQL. Promoting a settlement to Tier `5` (`City`) or Tier `6` (`Metropolis`) automatically uses the Tier `4` (`Town`) populated layout in `content.db`.

### Web Admin and GM Studio ([`docs/WEB_ADMIN.md`](WEB_ADMIN.md))

- **Single-Boot Passphrase Authentication (`Username: Admin`)**:
  - Startup generates a fresh passphrase exchanged via `POST /api/v1/admin/auth/login` for a scoped `HS256` JWT bound to the current `boot_id`, with an explicit **`END SESSION`** button in the top bar.
- **Constant `30 Hz` (`33 ms`) Map Streaming, `z=7` Tiles, and 4-Season Verra Calendar**:
  - Streams viewport entities every `33 ms` (`30 Hz`) without downsampling flicker, switches to `z=7` tiles at `zoom >= 3.0`, and includes an interactive popover (`VerraCalendarPopover`) for the `9x` clock and 360-day Verra Calendar (`Spring`, `Summer`, `Autumn`, `Winter`).
- **`Observability` Workspace**:
  - Consolidates the SRE Four Golden Signals, per-worker population tables, `<Service Name> (<Protocol>)` connection health cards, and live container logs with `Auto Tail` and `Auto Follow` controls.

---

## 2026-10-28 (`v0.9.35` -- 4-Tier Repository Layout, Native ICS `:50061` Character Creation & Live Play-Flow Gate)

- **4-Tier Directory Layout (`src/`, `build/`, `dist/`, `deploy/`)**:
  - Organized tracked source code under `src/`, compiler outputs under `build/`, compiled binaries and extracted game data under `dist/`, and active runtime state under `deploy/` with three root batch scripts (`start.bat`, `stop.bat`, `build.bat`) and `SandalLauncher.lnk`.
- **`SandalLauncher` and Host `LocalTetherListener` (`127.0.0.1:19021/udp`)**:
  - Added in-process UDP tether handshake handling and native `:50061` ICS TLS 1.3 in-game character creation.
- **Live Functional API and 8-Class `Lv 50` Bot Verification (`scripts/test_functional_apis.ps1`)**:
  - End-to-end verification across HTTP Auth (`:8080`), UDP Tether (`:19021`), ICS TLS 1.3 (`:50061`), and live UDP `:7777` `fake::Client` bot sessions inside `aoc-world`.
