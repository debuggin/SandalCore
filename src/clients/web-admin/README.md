# Web Admin Frontend

**Related Guides**: [Root Overview](../../../README.md) | [Web Admin Guide](../../../docs/WEB_ADMIN.md) | [Web Admin Architecture](../../../docs/WEB_ADMIN_ARCHITECTURE.md) | [UI Style Guide](../../../docs/UI_STYLE_GUIDE.md) | [Web Admin Container](../../server/services/cmd/web-admin/README.md)

`src/clients/web-admin` is the browser frontend for the SandalCore **Web Admin and GM Studio**, served by `aoc-web-admin` (`:8090`) and `aoc-ingress-proxy` (`https://localhost:8443/admin/`).

---

## Key Capabilities

- **Single-Boot Passphrase Sign-In**: Displays the **Admin Login Modal** (`Username: Admin`) when unauthenticated or when `AOC_ADMIN_BOOT_ID` changes after a server restart, exchanging the startup passphrase for a scoped `HS256` JWT, with an **`END SESSION`** button in the top bar.
- **Constant `30 Hz` (`33 ms`) Map Streaming**: Polls `/api/v1/world/state` every `33 ms` across all zoom levels (`z0..z7+`) with viewport bounding-box culling (`minX`, `minY`, `maxX`, `maxY`) and switches to `z=7` minimap tiles at `zoom >= 3.0`.
- **Mutually Exclusive Map Selection**: Clicking a map item opens one card at a time in the top-right inspector slot (`width: 360px`) for an **Entity** (Player, NPC, Monster, Mount, or Resource Node with `Resource Type` and `Quality / Rarity`), **City Node** (`Stage 0..6`, XP, mayor, and tax controls), **POI**, or **Picked Coordinate**.
- **Searchable Teleport and Spawn Modals**: Provides instant search modals for teleporting characters (`Teleport Player Here...`) and spawning NPCs or monsters from `npcs.tsv` (`Spawn NPC / Monster Here...`).
- **World Clock, 4-Season Verra Calendar, and Worker Controls**: Includes the top-bar `Time HH:MM:SS` popover (`VerraCalendarPopover`) for the `9x` clock and 360-day Verra Calendar (`Spring`, `Summer`, `Autumn`, `Winter`), plus the fixed-size `Visible Server Workers` panel (`450px x 208px`) with per-worker tick-rate selectors (`Dynamic (30-60 Hz)`, `0 Hz (Pause NPCs)`, and `10..60 Hz`).
- **`Observability` Workspace**: Displays the SRE Four Golden Signals, per-worker population tables, `<Service Name> (<Protocol>)` connection health cards, and live container logs with `Auto Tail` and `Auto Follow` toggles.

---

## Source Layout

| Path | Purpose |
|:---|:---|
| `src/main.tsx` | Top navigation bar (`Map` and `Observability` tabs), `StreamStatus` header, and workspace router. |
| `src/map/VerraMapCanvas.tsx` | Interactive HTML5 map canvas, top-left zoom and city quick-jump controls, top-right inspector slot, and bottom-right worker panel. |
| `src/map/verraTilePyramidRenderer.ts` | Renders `verra.pmtiles` map tiles (`z=7` at `zoom >= 3.0`), 3-pass ZOI borders, `1 km x 1 km` City Node OBBs, and entity markers. |
| `src/state/worldStreamStore.ts` | Constant `30 Hz` (`33 ms`) state poller, persistent entity store, and mutually exclusive map selection state. |
| `src/components/EntityInspector.tsx` | Inspector card for players, NPCs, monsters, mounts, resource nodes, and POIs, with one-click GM actions. |
| `src/components/NodeGovernanceAndSpawnModals.tsx` | City Node governance card (`Stage 0..6`) and searchable `Teleport Player Here...` and `Spawn NPC / Monster Here...` modals. |
| `src/components/VerraCalendarPopover.tsx` | Top-bar popover (`zIndex: 120`) for controlling the `9x` server clock and 360-day 4-season Verra Calendar. |
| `src/components/LogsAndContainersTab.tsx` | Four-zone **Observability** workspace (Golden Signals, Worker Shards, Container USE & Service Health, and Structured Logs). |
| `src/components/AdminLoginModal.tsx` | Single-boot passphrase login modal and `boot_id` session check. |

---

## Local Development and Testing

```powershell
# Run the unit and component test suite
npm --prefix src/clients/web-admin test -- --run

# Type-check and build the production bundle
npm --prefix src/clients/web-admin run build
```
