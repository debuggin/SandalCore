# Web Admin Server

**Related Guides**: [Root Overview](../../../../../README.md) | [Web Admin Guide](../../../../../docs/WEB_ADMIN.md) | [Web Admin Architecture](../../../../../docs/WEB_ADMIN_ARCHITECTURE.md) | [Web Admin Frontend](../../../../clients/web-admin/README.md) | [Gateway Service](../gateway/README.md)

`src/server/services/cmd/web-admin` (`aoc-web-admin`, listening on `:8090` HTTP) is the HTTP server that hosts the Web Admin frontend bundle, serves HTTP `206 Partial Content` byte-range requests for `verra.pmtiles` map tiles, and proxies `/api/*` requests to `aoc-gateway` (`:8080`) and `aoc-mesh-gateway` (`:8082`).

---

## Core Responsibilities

- **Static SPA Hosting**: Serves the compiled `src/clients/web-admin/dist` assets under `/` and `/admin/`.
- **PMTiles Range Serving (`/tiles/verra.pmtiles`)**: Handles HTTP `Range` (`206 Partial Content`) requests against `/app/content/verra.pmtiles` so the browser map canvas fetches only the visible `z0..z7` minimap tiles.
- **API Reverse Proxy (`/api/v1/*`)**: Forwards authentication, `30 Hz` world-state polling, Game Master commands, and container observability requests to `aoc-gateway` and `aoc-mesh-gateway`.

---

## Configuration

| Environment Variable | Default | Description |
|:---|:---|:---|
| `AOC_WEB_ADMIN_ADDR` | `:8090` | HTTP listen address inside `aoc-web-admin`. |
| `AOC_GATEWAY_URL` | `http://gateway:8080` | Upstream `aoc-gateway` URL for `/api/v1/*` routes. |
| `AOC_PMTILES_PATH` | `/app/content/verra.pmtiles` | Path to the extracted Verra map tile archive. |

---

## Testing

```powershell
.\sandal.ps1 test
```
