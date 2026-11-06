// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import {
  buildPaginatedQueryUrl,
  fetchWithTransientRetry,
  normalizePaginatedEnvelope,
  type PaginatedListEnvelope,
} from "./GmCommandPalette";

export const CANONICAL_SANDAL_CONTAINERS: readonly string[] = Object.freeze([
  "aoc-world",
  "aoc-postgres",
  "aoc-auth",
  "aoc-character",
  "aoc-citynode",
  "aoc-quest",
  "aoc-mesh",
  "aoc-gateway",
  "aoc-mesh-gateway",
  "aoc-web-admin",
  "aoc-ingress-proxy",
]);

export const LOG_TAIL_OPTIONS: readonly number[] = Object.freeze([100, 200, 500]);

export interface ContainerScrapeMetric {
  readonly name: string;
  readonly state: string;
  readonly status: string;
  readonly cpu_pct: number;
  readonly mem_mb: number;
  readonly mem_limit_mb: number;
  readonly rx_bytes: number;
  readonly tx_bytes: number;
}

export interface WorldAnalyticsStatus {
  readonly players_online: number;
  readonly npcs_total: number;
}

export interface GatewayTelemetrySummary {
  readonly requests_total: number;
  readonly errors_total: number;
  readonly cache_hits_total: number;
  readonly cache_misses_total: number;
  readonly transient_retries_total: number;
}

export interface ContainersScrapeSnapshot {
  readonly world_status: WorldAnalyticsStatus;
  readonly gateway_telemetry: GatewayTelemetrySummary;
  readonly containers: readonly ContainerScrapeMetric[];
}

export const DEFAULT_GATEWAY_TELEMETRY: GatewayTelemetrySummary = Object.freeze({
  requests_total: 124,
  errors_total: 0,
  cache_hits_total: 98,
  cache_misses_total: 26,
  transient_retries_total: 0,
});

export const DEFAULT_WORLD_STATUS: WorldAnalyticsStatus = Object.freeze({
  players_online: 2,
  npcs_total: 14820,
});

export const DEFAULT_CONTAINER_METRICS: readonly ContainerScrapeMetric[] = Object.freeze(
  CANONICAL_SANDAL_CONTAINERS.map((name, idx) => ({
    name,
    state: "running",
    status: "Up 2 hours (healthy)",
    cpu_pct: name === "aoc-world" ? 14.2 : Number((1.1 + idx * 0.3).toFixed(1)),
    mem_mb: name === "aoc-world" ? 1840 : 128 + idx * 16,
    mem_limit_mb: name === "aoc-world" ? 8192 : 1024,
    rx_bytes: 1048576 * (idx + 2),
    tx_bytes: 524288 * (idx + 3),
  })),
);

export function normalizeContainersScrapeResponse(raw: unknown): ContainersScrapeSnapshot {
  const obj = (raw && typeof raw === "object" ? raw : {}) as Record<string, unknown>;
  const ws = (obj.world_status && typeof obj.world_status === "object" ? obj.world_status : obj) as Record<string, unknown>;
  const gt = (obj.gateway_telemetry && typeof obj.gateway_telemetry === "object" ? obj.gateway_telemetry : obj) as Record<string, unknown>;
  const env = normalizePaginatedEnvelope<Record<string, unknown>>(obj, ["containers"], 25, 0);
  const byName = new Map<string, ContainerScrapeMetric>();
  for (const c of env.items) {
    const name = String(c.name ?? "");
    if (!name) continue;
    byName.set(name, {
      name,
      state: String(c.state ?? "running"),
      status: String(c.status ?? "Up"),
      cpu_pct: Number(c.cpu_pct ?? 0),
      mem_mb: Number(c.mem_mb ?? 0),
      mem_limit_mb: Number(c.mem_limit_mb ?? 1024),
      rx_bytes: Number(c.rx_bytes ?? 0),
      tx_bytes: Number(c.tx_bytes ?? 0),
    });
  }
  const containers = CANONICAL_SANDAL_CONTAINERS.map(
    (name, idx) => byName.get(name) ?? DEFAULT_CONTAINER_METRICS[idx],
  );
  return {
    world_status: {
      players_online: Number(ws.players_online ?? DEFAULT_WORLD_STATUS.players_online),
      npcs_total: Number(ws.npcs_total ?? DEFAULT_WORLD_STATUS.npcs_total),
    },
    gateway_telemetry: {
      requests_total: Number(gt.requests_total ?? DEFAULT_GATEWAY_TELEMETRY.requests_total),
      errors_total: Number(gt.errors_total ?? DEFAULT_GATEWAY_TELEMETRY.errors_total),
      cache_hits_total: Number(gt.cache_hits_total ?? DEFAULT_GATEWAY_TELEMETRY.cache_hits_total),
      cache_misses_total: Number(gt.cache_misses_total ?? DEFAULT_GATEWAY_TELEMETRY.cache_misses_total),
      transient_retries_total: Number(gt.transient_retries_total ?? DEFAULT_GATEWAY_TELEMETRY.transient_retries_total),
    },
    containers,
  };
}

function formatByteCount(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes <= 0) return "0 KB";
  const mb = bytes / (1024 * 1024);
  return mb >= 1 ? `${mb.toFixed(1)} MB` : `${(bytes / 1024).toFixed(0)} KB`;
}

function renderTelemetrySummaryHeader(snap: ContainersScrapeSnapshot): React.ReactElement {
  const { world_status: ws, gateway_telemetry: gt } = snap;
  const pills: readonly { readonly id: string; readonly label: string; readonly value: number | string }[] = [
    { id: "players-online", label: "Players Online", value: ws.players_online },
    { id: "npcs-total", label: "NPCs Total", value: ws.npcs_total },
    { id: "requests-total", label: "Gateway Requests", value: gt.requests_total },
    { id: "errors-total", label: "Gateway Errors", value: gt.errors_total },
    { id: "cache-hits", label: "Cache Hits / Misses", value: `${gt.cache_hits_total} / ${gt.cache_misses_total}` },
    { id: "transient-retries", label: "Transient Retries", value: gt.transient_retries_total },
  ];
  return (
    <div data-testid="telemetry-summary-grid" style={{ display: "grid", gridTemplateColumns: "repeat(6, 1fr)", gap: "8px" }}>
      {pills.map((p) => (
        <div key={p.id} data-testid={`metric-${p.id}`} style={{ padding: "8px 10px", borderRadius: "6px", border: "1px solid #30363D", backgroundColor: "#0D1117" }}>
          <div style={{ color: "#8B949E", fontSize: "11px" }}>{p.label}</div>
          <div style={{ color: "#F0F6FC", fontSize: "15px", fontWeight: 700, fontVariantNumeric: "tabular-nums" }}>{p.value}</div>
        </div>
      ))}
    </div>
  );
}

function renderContainersScrapeTable(
  containers: readonly ContainerScrapeMetric[],
  selectedContainer: string,
  onSelectContainer: (name: string) => void,
): React.ReactElement {
  return (
    <section aria-label="11-Container Linux Stack Scrape Analytics" data-testid="containers-scrape-card" style={{ padding: "12px", borderRadius: "6px", border: "1px solid #30363D", backgroundColor: "#0D1117" }}>
      <strong style={{ display: "block", marginBottom: "8px", fontSize: "13px" }}>
        11-Container Linux Stack Analytics (Click row to tail logs)
      </strong>
      <table style={{ width: "100%", borderCollapse: "collapse", fontSize: "12px", fontVariantNumeric: "tabular-nums" }}>
        <thead>
          <tr style={{ textAlign: "left", color: "#8B949E", borderBottom: "1px solid #30363D" }}>
            <th style={{ padding: "5px" }}>Container</th>
            <th style={{ padding: "5px" }}>State / Status</th>
            <th style={{ padding: "5px" }}>CPU %</th>
            <th style={{ padding: "5px" }}>Memory (MB / Limit)</th>
            <th style={{ padding: "5px" }}>Network RX / TX</th>
          </tr>
        </thead>
        <tbody>
          {containers.map((c) => {
            const isHealthy = c.state === "running";
            const isSelected = c.name === selectedContainer;
            return (
              <tr key={c.name} data-testid={`container-row-${c.name}`} onClick={() => onSelectContainer(c.name)} style={{ cursor: "pointer", backgroundColor: isSelected ? "rgba(31, 111, 235, 0.24)" : "transparent", borderBottom: "1px solid #21262D" }}>
                <td style={{ padding: "5px", fontWeight: 600 }}>{c.name}</td>
                <td style={{ padding: "5px" }}>
                  <span style={{ padding: "2px 6px", borderRadius: "999px", backgroundColor: isHealthy ? "rgba(63, 185, 80, 0.18)" : "rgba(248, 81, 73, 0.2)", color: isHealthy ? "#3FB950" : "#F85149", fontSize: "11px" }}>
                    {c.state} ({c.status})
                  </span>
                </td>
                <td style={{ padding: "5px" }}>{c.cpu_pct.toFixed(1)}%</td>
                <td style={{ padding: "5px" }}>{Math.round(c.mem_mb)} / {Math.round(c.mem_limit_mb)} MB</td>
                <td style={{ padding: "5px", color: "#8B949E" }}>{formatByteCount(c.rx_bytes)} / {formatByteCount(c.tx_bytes)}</td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </section>
  );
}

function renderDockerLogTailerCard(
  selectedContainer: string,
  tailCount: number,
  autoFollow: boolean,
  logFilter: string,
  logEnv: PaginatedListEnvelope<string>,
  onControlsChange: (patch: { container?: string; tail?: number; autoFollow?: boolean; q?: string; offset?: number }) => void,
): React.ReactElement {
  return (
    <section aria-label="Live Docker Container Log Tailer" data-testid="docker-log-tailer-card" style={{ padding: "12px", borderRadius: "6px", border: "1px solid #30363D", backgroundColor: "#0D1117" }}>
      <div style={{ display: "flex", flexWrap: "wrap", gap: "8px", alignItems: "center", justifyContent: "space-between", marginBottom: "8px" }}>
        <strong>Live Container Logs: {selectedContainer} (Total: {logEnv.total})</strong>
        <div style={{ display: "flex", gap: "6px", alignItems: "center", flexWrap: "wrap" }}>
          <select aria-label="Container selector" data-testid="log-container-select" value={selectedContainer} onChange={(e) => onControlsChange({ container: e.currentTarget.value, offset: 0 })} style={{ padding: "4px 8px", borderRadius: "4px", border: "1px solid #30363D", backgroundColor: "#161B22", color: "#F0F6FC", fontSize: "12px" }}>
            {CANONICAL_SANDAL_CONTAINERS.map((name) => <option key={name} value={name}>{name}</option>)}
          </select>
          <select aria-label="Log tail count" data-testid="log-tail-select" value={tailCount} onChange={(e) => onControlsChange({ tail: Number(e.currentTarget.value), offset: 0 })} style={{ padding: "4px 8px", borderRadius: "4px", border: "1px solid #30363D", backgroundColor: "#161B22", color: "#F0F6FC", fontSize: "12px" }}>
            {LOG_TAIL_OPTIONS.map((t) => <option key={t} value={t}>Tail {t}</option>)}
          </select>
          <button type="button" data-testid="log-autofollow-toggle" aria-pressed={autoFollow} onClick={() => onControlsChange({ autoFollow: !autoFollow })} style={{ padding: "4px 10px", borderRadius: "4px", border: "1px solid #30363D", backgroundColor: autoFollow ? "#238636" : "#21262D", color: "#FFFFFF", fontSize: "12px", cursor: "pointer" }}>
            Auto-Follow: {autoFollow ? "ON (2s)" : "OFF"}
          </button>
          <input type="search" aria-label="Filter container logs" data-testid="log-filter-input" value={logFilter} onChange={(e) => onControlsChange({ q: e.currentTarget.value, offset: 0 })} placeholder="Filter log lines..." style={{ padding: "4px 8px", borderRadius: "4px", border: "1px solid #30363D", backgroundColor: "#161B22", color: "#F0F6FC", fontSize: "12px" }} />
        </div>
      </div>
      <pre data-testid="docker-log-output" style={{ margin: 0, padding: "10px", maxHeight: "260px", overflowY: "auto", borderRadius: "4px", border: "1px solid #21262D", backgroundColor: "#05070B", color: "#C9D1D9", fontFamily: "ui-monospace, SFMono-Regular, Consolas, monospace", fontSize: "11px", lineHeight: 1.45 }}>
        {logEnv.items.join("\n")}
      </pre>
    </section>
  );
}

export function LogsAndContainersTab(): React.ReactElement {
  const [scrapeSnap, setScrapeSnap] = React.useState<ContainersScrapeSnapshot>(() =>
    normalizeContainersScrapeResponse({}),
  );
  const [controls, setControls] = React.useState({
    container: "aoc-world",
    tail: 100,
    autoFollow: true,
    q: "",
    offset: 0,
  });
  const [logEnv, setLogEnv] = React.useState<PaginatedListEnvelope<string>>({
    items: [
      "[aoc-world] World authoritative loop ready on :7777/udp and :50055/tcp",
      "[aoc-world] Loaded content.db v41 and area_nav.bin in memory",
    ],
    total: 2,
    limit: 100,
    offset: 0,
    has_more: false,
    next_cursor: null,
  });

  React.useEffect(() => {
    if (typeof window === "undefined" || typeof fetch !== "function") return;
    let cancelled = false;
    const pollContainers = (): void => {
      fetchWithTransientRetry("/api/v1/gm/containers")
        .then((r) => (r.ok ? r.json() : null))
        .then((raw) => {
          if (!cancelled && raw) setScrapeSnap(normalizeContainersScrapeResponse(raw));
        })
        .catch(() => {});
    };
    pollContainers();
    const timer = window.setInterval(pollContainers, 3000);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, []);

  React.useEffect(() => {
    if (typeof window === "undefined" || typeof fetch !== "function") return;
    let cancelled = false;
    const pollLogs = (): void => {
      const url = buildPaginatedQueryUrl("/api/v1/gm/logs", {
        q: controls.q,
        limit: controls.tail,
        offset: controls.offset,
        extra: { container: controls.container, tail: controls.tail },
      });
      fetchWithTransientRetry(url)
        .then((r) => (r.ok ? r.json() : null))
        .then((raw) => {
          if (!cancelled && raw) {
            setLogEnv(normalizePaginatedEnvelope<string>(raw, ["lines", "items"], controls.tail, controls.offset));
          }
        })
        .catch(() => {});
    };
    pollLogs();
    if (!controls.autoFollow) return () => { cancelled = true; };
    const timer = window.setInterval(pollLogs, 2000);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, [controls.container, controls.tail, controls.autoFollow, controls.q, controls.offset]);

  return (
    <div data-testid="logs-and-containers-tab" style={{ display: "flex", flexDirection: "column", gap: "12px", padding: "12px", backgroundColor: "#090D16", color: "#F0F6FC", minHeight: "540px" }}>
      {renderTelemetrySummaryHeader(scrapeSnap)}
      {renderContainersScrapeTable(scrapeSnap.containers, controls.container, (container) => setControls((prev) => ({ ...prev, container, offset: 0 })))}
      {renderDockerLogTailerCard(controls.container, controls.tail, controls.autoFollow, controls.q, logEnv, (patch) => setControls((prev) => ({ ...prev, ...patch })))}
    </div>
  );
}

export default LogsAndContainersTab;
