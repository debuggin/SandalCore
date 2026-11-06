// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import {
  DEFAULT_REGIONAL_WORLD_WORKERS,
  useWorldStreamStore,
  type WorldWorkerInfo,
  type WorkerScopeKind,
} from "../state/worldStreamStore";
import {
  buildPaginatedQueryUrl,
  fetchWithTransientRetry,
  normalizePaginatedEnvelope,
  type PaginatedListEnvelope,
} from "./GmCommandPalette";
import { SANDAL_LAUNCHER_TOKENS } from "./PlayersAndSpawnerTab";

export const CANONICAL_SANDAL_CONTAINERS: readonly string[] = Object.freeze([
  "aoc-world-riverlands",
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

export const REGIONAL_WORKER_CONTAINERS: readonly string[] = Object.freeze([
  "aoc-world-riverlands",
  "aoc-world-sandsquall",
  "aoc-world-jundark",
  "aoc-world-anvils",
  "aoc-world-turquoise-sea",
]);

export const LOG_CONTAINERS: readonly { readonly name: string; readonly label: string }[] = Object.freeze([
  { name: "aoc-world-riverlands", label: "world-riverlands (C++23 Riverlands Zone)" },
  { name: "aoc-world-sandsquall", label: "world-sandsquall (C++23 Sandsquall Zone)" },
  { name: "aoc-world-jundark", label: "world-jundark (C++23 Jundark Zone)" },
  { name: "aoc-world-anvils", label: "world-anvils (C++23 Anvils Zone)" },
  { name: "aoc-world-turquoise-sea", label: "world-turquoise-sea (C++23 Turquoise Sea Zone)" },
]);

export const LOG_TAIL_OPTIONS: readonly number[] = Object.freeze([100, 200, 500]);

export interface PrometheusTargetStatus {
  readonly name: string;
  readonly url: string;
  readonly status: string;
  readonly latency_ms: number;
  readonly exported_metrics: number;
  readonly families_count: number;
}

export interface PrometheusScrapeReport {
  readonly scraped_at_ms: number;
  readonly targets: readonly PrometheusTargetStatus[];
  readonly mesh_gateway_metrics: Readonly<Record<string, number>>;
  readonly world_metrics: {
    readonly connected: boolean;
    readonly tick_hz: number;
    readonly tick_p50_ms: number;
    readonly tick_p99_ms: number;
    readonly npc_count: number;
    readonly resource_node_count: number;
    readonly city_node_count: number;
    readonly active_players: number;
  };
}

export interface ContainerScrapeMetric {
  readonly name: string;
  readonly state: string;
  readonly status: string;
  readonly cpu_pct: number;
  readonly mem_mb: number;
  readonly mem_limit_mb: number;
  readonly mem_pct: number;
  readonly rx_bytes: number;
  readonly tx_bytes: number;
  readonly scrape_target: string;
  readonly scrape_status: string;
  readonly scrape_latency_ms: number;
  readonly exported_metrics: number;
  readonly requests_total: number;
  readonly errors_total: number;
  readonly active_connections: number;
}

export interface WorldAnalyticsStatus {
  readonly players_online: number;
  readonly npcs_total: number;
  readonly monsters_total: number;
  readonly resource_nodes_total: number;
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
  readonly prometheus: PrometheusScrapeReport;
  readonly workers: readonly WorldWorkerInfo[];
  readonly containers: readonly ContainerScrapeMetric[];
}

export const DEFAULT_PROMETHEUS_TARGETS: readonly PrometheusTargetStatus[] = Object.freeze([
  {
    name: "ingress-proxy",
    url: "ingress-proxy:9901/stats/prometheus",
    status: "UP",
    latency_ms: 1.2,
    exported_metrics: 148,
    families_count: 34,
  },
  {
    name: "mesh-gateway",
    url: "mesh-gateway:8082/metrics",
    status: "UP",
    latency_ms: 0.9,
    exported_metrics: 64,
    families_count: 18,
  },
  {
    name: "world-stream",
    url: "world:50062/WorldStreamService",
    status: "UP",
    latency_ms: 0.8,
    exported_metrics: 8,
    families_count: 8,
  },
]);

export const EMPTY_GATEWAY_TELEMETRY: GatewayTelemetrySummary = Object.freeze({
  requests_total: 0,
  errors_total: 0,
  cache_hits_total: 0,
  cache_misses_total: 0,
  transient_retries_total: 0,
});

export const EMPTY_WORLD_STATUS: WorldAnalyticsStatus = Object.freeze({
  players_online: 0,
  npcs_total: 0,
  monsters_total: 0,
  resource_nodes_total: 0,
});

function buildEmptyContainerMetric(name: string): ContainerScrapeMetric {
  return {
    name,
    state: "unknown",
    status: "Awaiting scrape",
    cpu_pct: 0,
    mem_mb: 0,
    mem_limit_mb: 0,
    mem_pct: 0,
    rx_bytes: 0,
    tx_bytes: 0,
    scrape_target: `${name}:metrics`,
    scrape_status: "UP",
    scrape_latency_ms: 0,
    exported_metrics: 0,
    requests_total: 0,
    errors_total: 0,
    active_connections: 0,
  };
}

function normalizePrometheusReport(
  rawProm: unknown,
  ws: Record<string, unknown>,
): PrometheusScrapeReport {
  const p = (rawProm && typeof rawProm === "object" ? rawProm : {}) as Record<string, unknown>;
  const rawTargets = Array.isArray(p.targets)
    ? (p.targets as readonly Record<string, unknown>[])
    : [];
  const targets: readonly PrometheusTargetStatus[] =
    rawTargets.length > 0
      ? rawTargets.map((t) => ({
          name: String(t.name ?? "target"),
          url: String(t.url ?? ""),
          status: String(t.status ?? "UP").toUpperCase(),
          latency_ms: Number(t.latency_ms ?? 0),
          exported_metrics: Number(t.exported_metrics ?? 0),
          families_count: Number(t.families_count ?? 0),
        }))
      : DEFAULT_PROMETHEUS_TARGETS;
  const mgRaw = (
    p.mesh_gateway_metrics && typeof p.mesh_gateway_metrics === "object"
      ? p.mesh_gateway_metrics
      : {}
  ) as Record<string, unknown>;
  const mesh_gateway_metrics: Record<string, number> = {};
  for (const [k, v] of Object.entries(mgRaw)) {
    mesh_gateway_metrics[k] = Number(v ?? 0);
  }
  const wm = (
    p.world_metrics && typeof p.world_metrics === "object" ? p.world_metrics : {}
  ) as Record<string, unknown>;
  return {
    scraped_at_ms: Number(p.scraped_at_ms ?? Date.now()),
    targets,
    mesh_gateway_metrics,
    world_metrics: {
      connected: Boolean(wm.connected ?? ws.connected ?? true),
      tick_hz: Number(wm.tick_hz ?? 30.0),
      tick_p50_ms: Number(wm.tick_p50_ms ?? ws.tick_ms ?? 0),
      tick_p99_ms: Number(wm.tick_p99_ms ?? ws.publish_max_ms ?? 0),
      npc_count: Number(wm.npc_count ?? ws.npcs_total ?? ws.npcs ?? 0),
      resource_node_count: Number(wm.resource_node_count ?? ws.resource_nodes_total ?? ws.resource_nodes ?? 0),
      city_node_count: Number(wm.city_node_count ?? 26),
      active_players: Number(wm.active_players ?? ws.players_online ?? ws.players ?? 0),
    },
  };
}

function normalizeWorldWorkers(
  rawWorkers: unknown,
  byName: ReadonlyMap<string, ContainerScrapeMetric>,
  ws: Record<string, unknown>,
  prom: PrometheusScrapeReport,
): readonly WorldWorkerInfo[] {
  const byId = new Map<string, WorldWorkerInfo>();
  for (const def of DEFAULT_REGIONAL_WORLD_WORKERS) {
    const cm = byName.get(def.container_name);
    const isPrimary = def.worker_id === "world-riverlands";
    const npcs = isPrimary ? Number(ws.npcs_total ?? ws.npcs ?? def.npcs) : def.npcs;
    const monsters = isPrimary ? Number(ws.monsters_total ?? ws.monsters ?? def.monsters) : def.monsters;
    const resources = isPrimary ? Number(ws.resource_nodes_total ?? ws.resource_nodes ?? def.resource_nodes) : def.resource_nodes;
    const players = isPrimary ? Number(ws.players_online ?? ws.players ?? def.active_players) : def.active_players;
    byId.set(def.worker_id, {
      ...def,
      status: cm?.state === "running" || isPrimary ? "UP" : def.status,
      tick_hz: isPrimary ? prom.world_metrics.tick_hz : def.tick_hz,
      npc_count: npcs,
      monster_count: monsters,
      resource_node_count: resources,
      player_count: players,
      npcs,
      monsters,
      resource_nodes: resources,
      active_players: players,
    });
  }
  const items = Array.isArray(rawWorkers) ? (rawWorkers as readonly Record<string, unknown>[]) : [];
  for (const w of items) {
    const id = String(w.worker_id ?? w.id ?? "");
    if (!id) continue;
    const prev = byId.get(id);
    const kind = String(w.scope_kind ?? prev?.scope_kind ?? "region") as WorkerScopeKind;
    const npcs = Number(w.npcs ?? w.npc_count ?? prev?.npcs ?? 0);
    const monsters = Number(w.monsters ?? w.monster_count ?? prev?.monsters ?? 0);
    const resources = Number(w.resource_nodes ?? w.resource_node_count ?? prev?.resource_nodes ?? 0);
    const players = Number(w.active_players ?? w.player_count ?? w.players ?? prev?.active_players ?? 0);
    byId.set(id, {
      worker_id: id,
      scope_kind: kind === "zoi" || kind === "node" ? kind : "region",
      scope_key: String(w.scope_key ?? prev?.scope_key ?? id),
      region_label: String(w.region_label ?? w.label ?? prev?.region_label ?? id),
      container_name: String(w.container_name ?? w.container ?? prev?.container_name ?? `aoc-${id}`),
      status: String(w.status ?? prev?.status ?? "UP").toUpperCase(),
      tick_hz: Number(w.tick_hz ?? prev?.tick_hz ?? 30),
      npc_count: npcs,
      monster_count: monsters,
      resource_node_count: resources,
      player_count: players,
      npcs,
      monsters,
      resource_nodes: resources,
      active_players: players,
    });
  }
  return Array.from(byId.values());
}

function parseContainerItem(c: Record<string, unknown>): ContainerScrapeMetric {
  const name = String(c.name ?? "");
  const memMb = Number(c.mem_mb ?? c.memory_mb ?? 0);
  const memLim = Number(c.mem_limit_mb ?? c.memory_limit_mb ?? 0);
  return {
    name,
    state: String(c.state ?? "running"),
    status: String(c.status ?? "Up"),
    cpu_pct: Number(c.cpu_pct ?? c.cpu_percent ?? 0),
    mem_mb: memMb,
    mem_limit_mb: memLim,
    mem_pct: Number(c.mem_pct ?? c.memory_pct ?? (memLim > 0 ? (memMb / memLim) * 100 : 0)),
    rx_bytes: Number(c.rx_bytes ?? c.net_rx_bytes ?? 0),
    tx_bytes: Number(c.tx_bytes ?? c.net_tx_bytes ?? 0),
    scrape_target: String(c.scrape_target ?? `${name}:metrics`),
    scrape_status: String(c.scrape_status ?? (c.state === "running" ? "UP" : "DOWN")).toUpperCase(),
    scrape_latency_ms: Number(c.scrape_latency_ms ?? 0),
    exported_metrics: Number(c.exported_metrics ?? 0),
    requests_total: Number(c.requests_total ?? 0),
    errors_total: Number(c.errors_total ?? 0),
    active_connections: Number(c.active_connections ?? 0),
  };
}

export function normalizeContainersScrapeResponse(
  raw: unknown,
): ContainersScrapeSnapshot {
  const obj = (raw && typeof raw === "object" ? raw : {}) as Record<string, unknown>;
  const ws = (
    obj.world_status && typeof obj.world_status === "object" ? obj.world_status : obj
  ) as Record<string, unknown>;
  const gt = (
    obj.gateway_telemetry && typeof obj.gateway_telemetry === "object"
      ? obj.gateway_telemetry
      : obj
  ) as Record<string, unknown>;
  const env = normalizePaginatedEnvelope<Record<string, unknown>>(obj, ["containers"], 25, 0);
  const byName = new Map<string, ContainerScrapeMetric>();
  for (const c of env.items) {
    const parsed = parseContainerItem(c);
    if (parsed.name) byName.set(parsed.name, parsed);
  }
  const extraContainers = Array.from(byName.values()).filter(
    (c) => !CANONICAL_SANDAL_CONTAINERS.includes(c.name),
  );
  const containers = [
    ...CANONICAL_SANDAL_CONTAINERS.map((name) => byName.get(name) ?? buildEmptyContainerMetric(name)),
    ...extraContainers,
  ];
  const prometheus = normalizePrometheusReport(obj.prometheus, ws);
  return {
    world_status: {
      players_online: Number(ws.players_online ?? ws.players ?? 0),
      npcs_total: Number(ws.npcs_total ?? ws.npcs ?? 0),
      monsters_total: Number(ws.monsters_total ?? ws.monsters ?? 0),
      resource_nodes_total: Number(ws.resource_nodes_total ?? ws.resource_nodes ?? 0),
    },
    gateway_telemetry: {
      requests_total: Number(gt.requests_total ?? 0),
      errors_total: Number(gt.errors_total ?? 0),
      cache_hits_total: Number(gt.cache_hits_total ?? 0),
      cache_misses_total: Number(gt.cache_misses_total ?? 0),
      transient_retries_total: Number(gt.transient_retries_total ?? 0),
    },
    prometheus,
    workers: normalizeWorldWorkers(obj.workers, byName, ws, prometheus),
    containers,
  };
}

function formatByteCount(bytes: number): string {
  const safe = Number(bytes ?? 0);
  if (!Number.isFinite(safe) || safe <= 0) return "0 KB";
  const mb = safe / (1024 * 1024);
  return mb >= 1 ? `${mb.toFixed(1)} MB` : `${(safe / 1024).toFixed(0)} KB`;
}

const PANEL_STYLE: React.CSSProperties = {
  padding: "14px",
  borderRadius: "12px",
  border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
  background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush,
  backdropFilter: "blur(16px)",
};

const SELECT_INPUT_STYLE: React.CSSProperties = {
  minHeight: "32px",
  padding: "4px 8px",
  borderRadius: "6px",
  border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
  backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush,
  color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100,
  fontSize: "12px",
};

function renderTelemetrySummaryHeader(snap: ContainersScrapeSnapshot): React.ReactElement {
  const { world_status: ws, gateway_telemetry: gt, prometheus: prom } = snap;
  const totalSeries = prom.targets.reduce((acc, t) => acc + Number(t.exported_metrics ?? 0), 0);
  const totalFamilies = prom.targets.reduce((acc, t) => acc + Number(t.families_count ?? 0), 0);
  const udpFrames =
    Number(prom.mesh_gateway_metrics.udp_frames_rx_total ?? 0) +
    Number(prom.mesh_gateway_metrics.udp_frames_tx_total ?? 0) +
    Number(prom.mesh_gateway_metrics.aoc_mesh_udp_packets_total ?? 0);
  const pills: readonly { readonly id: string; readonly label: string; readonly value: number | string }[] = [
    { id: "players-online", label: "Players Online", value: ws.players_online },
    { id: "npcs-total", label: "NPCs / Monsters", value: `${ws.npcs_total} / ${ws.monsters_total}` },
    { id: "resources-total", label: "Resource Nodes", value: ws.resource_nodes_total },
    { id: "requests-total", label: "Gateway Requests", value: gt.requests_total },
    { id: "errors-total", label: "Gateway Errors", value: gt.errors_total },
    { id: "cache-hits", label: "Cache Hits / Misses", value: `${gt.cache_hits_total} / ${gt.cache_misses_total}` },
    {
      id: "world-tick-hz",
      label: "World Tick (Hz / p50 / p99)",
      value: `${Number(prom.world_metrics.tick_hz ?? 30).toFixed(1)} Hz (${Number(prom.world_metrics.tick_p50_ms ?? 0).toFixed(1)} / ${Number(prom.world_metrics.tick_p99_ms ?? 0).toFixed(1)} ms)`,
    },
    { id: "prom-series", label: "OpenTelemetry /metrics Families / Series", value: `${totalFamilies} / ${totalSeries} (UDP: ${udpFrames})` },
  ];
  return (
    <div data-testid="telemetry-summary-grid" style={{ display: "grid", gridTemplateColumns: "repeat(4, 1fr)", gap: "10px" }}>
      {pills.map((p) => (
        <div key={p.id} data-testid={`metric-${p.id}`} style={{ ...PANEL_STYLE, padding: "10px 12px", borderRadius: "10px" }}>
          <div style={{ color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, fontSize: "11px" }}>{p.label}</div>
          <div style={{ color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500, fontSize: "15px", fontWeight: 700, fontVariantNumeric: "tabular-nums" }}>
            {p.value}
          </div>
        </div>
      ))}
    </div>
  );
}

function renderPrometheusTargetsCard(prom: PrometheusScrapeReport): React.ReactElement {
  return (
    <section aria-label="OpenTelemetry /metrics Scrape Telemetry" data-testid="prometheus-scrape-card" style={PANEL_STYLE}>
      <strong style={{ display: "block", marginBottom: "8px", fontSize: "12px", letterSpacing: "0.08em", textTransform: "uppercase", color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 }}>
        OpenTelemetry-Compatible /metrics Service Endpoints
      </strong>
      <div style={{ display: "grid", gridTemplateColumns: "repeat(3, 1fr)", gap: "10px" }}>
        {prom.targets.map((t) => {
          const isUp = t.status === "UP";
          return (
            <div key={t.name} data-testid={`prom-target-${t.name}`} style={{ padding: "8px 10px", borderRadius: "8px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush, display: "flex", justifyContent: "space-between", alignItems: "center", gap: "8px" }}>
              <div>
                <div style={{ fontWeight: 700, fontSize: "12px", color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100 }}>
                  {t.name} ({t.url})
                </div>
                <div style={{ fontSize: "11px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>
                  {Number(t.families_count ?? 0)} families | {Number(t.exported_metrics ?? 0)} series | {Number(t.latency_ms ?? 0).toFixed(1)} ms
                </div>
              </div>
              <span style={{ padding: "2px 8px", borderRadius: "999px", fontSize: "11px", fontWeight: 700, backgroundColor: isUp ? "rgba(32, 216, 206, 0.16)" : "rgba(224, 82, 88, 0.18)", color: isUp ? SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500 : SANDAL_LAUNCHER_TOKENS.VerraRuinCrimson500 }}>
                {t.status}
              </span>
            </div>
          );
        })}
      </div>
    </section>
  );
}

function renderContainerTableRow(
  c: ContainerScrapeMetric,
  selectedContainer: string,
  onSelectContainer: (name: string) => void,
): React.ReactElement {
  const isHealthy = c.state === "running";
  const isScrapeUp = c.scrape_status === "UP";
  const isSelected = c.name === selectedContainer;
  return (
    <tr
      key={c.name}
      data-testid={`container-row-${c.name}`}
      onClick={() => onSelectContainer(c.name)}
      style={{ cursor: "pointer", backgroundColor: isSelected ? "rgba(232, 178, 90, 0.14)" : "transparent", borderBottom: "1px solid rgba(140, 108, 66, 0.22)" }}
    >
      <td style={{ padding: "7px 6px", fontWeight: 600, color: isSelected ? SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100 }}>
        {c.name}
      </td>
      <td style={{ padding: "7px 6px" }}>
        <span style={{ padding: "2px 8px", borderRadius: "999px", backgroundColor: isHealthy ? "rgba(32, 216, 206, 0.16)" : "rgba(184, 196, 212, 0.16)", color: isHealthy ? SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500 : SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, fontSize: "11px" }}>
          {c.state} ({c.status})
        </span>
      </td>
      <td style={{ padding: "7px 6px" }}>
        <span style={{ color: isScrapeUp ? SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500 : SANDAL_LAUNCHER_TOKENS.VerraRuinCrimson500, fontWeight: 700, fontSize: "11px" }}>
          {c.scrape_status || "UP"}
        </span>
      </td>
      <td style={{ padding: "7px 6px" }}>{Number(c.cpu_pct ?? 0).toFixed(2)}%</td>
      <td style={{ padding: "7px 6px" }}>
        {Number(c.mem_mb ?? 0).toFixed(1)} / {Math.round(Number(c.mem_limit_mb ?? 0))} MB ({Number(c.mem_pct ?? 0).toFixed(1)}%)
      </td>
      <td style={{ padding: "7px 6px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>
        {formatByteCount(c.rx_bytes)} / {formatByteCount(c.tx_bytes)}
      </td>
      <td style={{ padding: "7px 6px" }}>
        {Math.round(Number(c.requests_total ?? 0))} / {Math.round(Number(c.errors_total ?? 0))} / {Math.round(Number(c.active_connections ?? 0))}
      </td>
      <td style={{ padding: "7px 6px" }}>{Number(c.scrape_latency_ms ?? 0).toFixed(1)} ms</td>
      <td style={{ padding: "7px 6px" }}>
        <button
          type="button"
          data-testid={`tail-logs-btn-${c.name}`}
          onClick={(e) => {
            e.stopPropagation();
            onSelectContainer(c.name);
          }}
          style={{ minHeight: "26px", padding: "2px 8px", borderRadius: "6px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: isSelected ? SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: isSelected ? SANDAL_LAUNCHER_TOKENS.VerraObsidian100 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "11px", fontWeight: 600, cursor: "pointer" }}
        >
          Logs
        </button>
      </td>
    </tr>
  );
}

function renderWorldAuthorityWorkersCard(
  workers: readonly WorldWorkerInfo[],
  selectedContainer: string,
  onSelectContainer: (name: string) => void,
): React.ReactElement {
  const headers = ["Worker ID", "Scope", "Container", "Status", "NPCs / Monsters", "Resources", "Players", "Tick Hz", "Logs"];
  return (
    <section aria-label="World Authority Workers" data-testid="world-authority-workers-card" style={PANEL_STYLE}>
      <strong style={{ display: "block", marginBottom: "10px", fontSize: "13px", letterSpacing: "0.08em", textTransform: "uppercase", color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 }}>
        World Authority Workers (Region / ZOI / Node Shards)
      </strong>
      <table style={{ width: "100%", borderCollapse: "collapse", fontSize: "12px", fontVariantNumeric: "tabular-nums" }}>
        <thead>
          <tr style={{ textAlign: "left", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, borderBottom: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}` }}>
            {headers.map((h) => (<th key={h} style={{ padding: "6px" }}>{h}</th>))}
          </tr>
        </thead>
        <tbody>
          {workers.map((w) => {
            const isUp = w.status === "UP";
            const isSel = w.container_name === selectedContainer;
            const scopeLabel = `${w.scope_kind.toUpperCase()}: ${w.region_label}`;
            return (
              <tr key={w.worker_id} data-testid={`worker-row-${w.worker_id}`} onClick={() => onSelectContainer(w.container_name)} style={{ cursor: "pointer", backgroundColor: isSel ? "rgba(232, 178, 90, 0.14)" : "transparent", borderBottom: "1px solid rgba(140, 108, 66, 0.22)" }}>
                <td style={{ padding: "7px 6px", fontWeight: 700, color: isSel ? SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100 }}>{w.worker_id}</td>
                <td style={{ padding: "7px 6px", color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400 }}>{scopeLabel}</td>
                <td style={{ padding: "7px 6px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>{w.container_name}</td>
                <td style={{ padding: "7px 6px" }}><span style={{ padding: "2px 8px", borderRadius: "999px", fontSize: "11px", fontWeight: 700, backgroundColor: isUp ? "rgba(32, 216, 206, 0.16)" : "rgba(224, 82, 88, 0.18)", color: isUp ? SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500 : SANDAL_LAUNCHER_TOKENS.VerraRuinCrimson500 }}>{w.status}</span></td>
                <td style={{ padding: "7px 6px" }}>{w.npcs} / {w.monsters}</td>
                <td style={{ padding: "7px 6px" }}>{w.resource_nodes}</td>
                <td style={{ padding: "7px 6px" }}>{w.active_players}</td>
                <td style={{ padding: "7px 6px" }}>{Number(w.tick_hz ?? 30).toFixed(1)} Hz</td>
                <td style={{ padding: "7px 6px" }}>
                  <button type="button" data-testid={`worker-logs-btn-${w.worker_id}`} onClick={(e) => { e.stopPropagation(); onSelectContainer(w.container_name); }} style={{ minHeight: "26px", padding: "2px 8px", borderRadius: "6px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: isSel ? SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: isSel ? SANDAL_LAUNCHER_TOKENS.VerraObsidian100 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "11px", fontWeight: 600, cursor: "pointer" }}>
                    Logs
                  </button>
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </section>
  );
}

function renderContainersScrapeTable(
  containers: readonly ContainerScrapeMetric[],
  selectedContainer: string,
  onSelectContainer: (name: string) => void,
): React.ReactElement {
  const headers = ["Container", "State / Status", "Scrape", "CPU %", "Memory (MB / %)", "Net RX / TX", "Reqs / Errs / Conns", "Latency", "Logs"];
  return (
    <section aria-label="15-Container Linux Stack Scrape Analytics" data-testid="containers-scrape-card" style={PANEL_STYLE}>
      <strong style={{ display: "block", marginBottom: "10px", fontSize: "13px", letterSpacing: "0.08em", textTransform: "uppercase", color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 }}>
        15-Container Linux Stack Scrape Analytics (Click row or Logs to inspect below)
      </strong>
      <table style={{ width: "100%", borderCollapse: "collapse", fontSize: "12px", fontVariantNumeric: "tabular-nums" }}>
        <thead>
          <tr style={{ textAlign: "left", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, borderBottom: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}` }}>
            {headers.map((h) => (
              <th key={h} style={{ padding: "6px" }}>{h}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {containers.map((c) => renderContainerTableRow(c, selectedContainer, onSelectContainer))}
        </tbody>
      </table>
    </section>
  );
}

interface DockerLogTailerProps {
  readonly selectedContainer: string;
  readonly tailCount: number;
  readonly autoFollow: boolean;
  readonly logFilter: string;
  readonly logEnv: PaginatedListEnvelope<string>;
}

function renderDockerLogTailerCard(
  props: DockerLogTailerProps,
  onControlsChange: (patch: { container?: string; tail?: number; autoFollow?: boolean; q?: string; offset?: number }) => void,
): React.ReactElement {
  const selectableContainers = Array.from(new Set([...CANONICAL_SANDAL_CONTAINERS, ...REGIONAL_WORKER_CONTAINERS, props.selectedContainer]));
  return (
    <section aria-label="Live Docker Container Log Tailer" data-testid="docker-log-tailer-card" style={PANEL_STYLE}>
      <div style={{ display: "flex", flexWrap: "wrap", gap: "8px", alignItems: "center", justifyContent: "space-between", marginBottom: "10px" }}>
        <strong style={{ color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500, fontSize: "13px" }}>
          Container Logs: {props.selectedContainer} ({props.logEnv.total} lines)
        </strong>
        <div style={{ display: "flex", gap: "6px", alignItems: "center", flexWrap: "wrap" }}>
          <select
            aria-label="Container selector"
            data-testid="log-container-select"
            value={props.selectedContainer}
            onChange={(e) => onControlsChange({ container: e.currentTarget.value, offset: 0 })}
            style={SELECT_INPUT_STYLE}
          >
            {selectableContainers.map((name) => (
              <option key={name} value={name}>{name}</option>
            ))}
          </select>
          <select
            aria-label="Log tail count"
            data-testid="log-tail-select"
            value={props.tailCount}
            onChange={(e) => onControlsChange({ tail: Number(e.currentTarget.value), offset: 0 })}
            style={SELECT_INPUT_STYLE}
          >
            {LOG_TAIL_OPTIONS.map((t) => (
              <option key={t} value={t}>Tail {t}</option>
            ))}
          </select>
          <button
            type="button"
            data-testid="log-autofollow-toggle"
            aria-pressed={props.autoFollow}
            onClick={() => onControlsChange({ autoFollow: !props.autoFollow })}
            style={{ ...SELECT_INPUT_STYLE, padding: "4px 10px", backgroundColor: props.autoFollow ? "rgba(32, 216, 206, 0.2)" : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: props.autoFollow ? SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontWeight: 600, cursor: "pointer" }}
          >
            Auto-Follow: {props.autoFollow ? "ON" : "OFF"}
          </button>
          <input
            type="search"
            aria-label="Filter container logs"
            data-testid="log-filter-input"
            value={props.logFilter}
            onChange={(e) => onControlsChange({ q: e.currentTarget.value, offset: 0 })}
            placeholder="Filter log lines..."
            style={SELECT_INPUT_STYLE}
          />
        </div>
      </div>
      <pre
        data-testid="docker-log-output"
        style={{ margin: 0, padding: "12px", maxHeight: "360px", overflowY: "auto", borderRadius: "8px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: "#06080E", color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontFamily: "ui-monospace, SFMono-Regular, Consolas, monospace", fontSize: "11px", lineHeight: 1.5 }}
      >
        {props.logEnv.items.length > 0 ? props.logEnv.items.join("\n") : "No log lines received yet."}
      </pre>
    </section>
  );
}

export interface LogsAndContainersTabProps {
  readonly mode?: "analytics" | "logs" | "both";
}

function useContainersScrapePolling(
  mode: "analytics" | "logs" | "both",
  onWorkersUpdate: (workers: readonly WorldWorkerInfo[]) => void,
): ContainersScrapeSnapshot {
  const [scrapeSnap, setScrapeSnap] = React.useState<ContainersScrapeSnapshot>(() =>
    normalizeContainersScrapeResponse({}),
  );
  React.useEffect(() => {
    if (typeof window === "undefined" || typeof fetch !== "function" || mode === "logs") return;
    let cancelled = false;
    const pollContainers = (): void => {
      fetchWithTransientRetry("/api/v1/gm/containers")
        .then((r) => (r.ok ? r.json() : null))
        .then((raw) => {
          if (cancelled || !raw) return;
          const next = normalizeContainersScrapeResponse(raw);
          setScrapeSnap(next);
          onWorkersUpdate(next.workers);
        })
        .catch(() => {});
    };
    pollContainers();
    const timer = window.setInterval(pollContainers, 3000);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, [mode]);
  return scrapeSnap;
}

function useContainerLogsPolling(
  mode: "analytics" | "logs" | "both",
  selectedContainer: string,
  logQuery: string,
  tail: number,
  autoFollow: boolean,
): { readonly logEnv: PaginatedListEnvelope<string>; readonly setOffset: (n: number) => void } {
  const [offset, setOffset] = React.useState(0);
  const [logEnv, setLogEnv] = React.useState<PaginatedListEnvelope<string>>({
    items: [],
    total: 0,
    limit: 100,
    offset: 0,
    has_more: false,
    next_cursor: null,
  });
  React.useEffect(() => {
    if (typeof window === "undefined" || typeof fetch !== "function" || mode === "analytics") return;
    let cancelled = false;
    const pollLogs = (): void => {
      const url = buildPaginatedQueryUrl("/api/v1/gm/logs", {
        q: logQuery,
        limit: tail,
        offset,
        extra: { container: selectedContainer, tail },
      });
      fetchWithTransientRetry(url)
        .then((r) => (r.ok ? r.json() : null))
        .then((raw) => {
          if (!cancelled && raw) setLogEnv(normalizePaginatedEnvelope<string>(raw, ["lines", "items"], tail, offset));
        })
        .catch(() => {});
    };
    pollLogs();
    if (!autoFollow) return () => { cancelled = true; };
    const timer = window.setInterval(pollLogs, 2000);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, [mode, selectedContainer, tail, autoFollow, logQuery, offset]);
  return { logEnv, setOffset };
}

export function LogsAndContainersTab(props?: LogsAndContainersTabProps): React.ReactElement {
  const mode = props?.mode ?? "both";
  const storeSnap = useWorldStreamStore();
  const [tail, setTail] = React.useState(100);
  const [autoFollow, setAutoFollow] = React.useState(true);
  const scrapeSnap = useContainersScrapePolling(mode, storeSnap.setWorkers);
  const { logEnv, setOffset } = useContainerLogsPolling(mode, storeSnap.selectedContainer, storeSnap.logQuery, tail, autoFollow);
  const showAnalytics = mode === "analytics" || mode === "both";
  const showLogs = mode === "logs" || mode === "both";
  const selectContainer = (container: string): void => {
    storeSnap.setSelectedContainer(container);
    setOffset(0);
  };

  return (
    <div data-testid="logs-and-containers-tab" style={{ display: "flex", flexDirection: "column", gap: "12px", padding: "16px", backgroundColor: SANDAL_LAUNCHER_TOKENS.VerraObsidian100, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, minHeight: "540px" }}>
      {showAnalytics ? renderTelemetrySummaryHeader(scrapeSnap) : null}
      {showAnalytics ? renderPrometheusTargetsCard(scrapeSnap.prometheus) : null}
      {showAnalytics ? renderWorldAuthorityWorkersCard(scrapeSnap.workers, storeSnap.selectedContainer, selectContainer) : null}
      {showAnalytics ? renderContainersScrapeTable(scrapeSnap.containers, storeSnap.selectedContainer, selectContainer) : null}
      {showLogs
        ? renderDockerLogTailerCard(
            { selectedContainer: storeSnap.selectedContainer, tailCount: tail, autoFollow, logFilter: storeSnap.logQuery, logEnv },
            (patch) => {
              if (patch.container !== undefined) storeSnap.setSelectedContainer(patch.container);
              if (patch.q !== undefined) storeSnap.setLogQuery(patch.q);
              if (patch.tail !== undefined) setTail(patch.tail);
              if (patch.autoFollow !== undefined) setAutoFollow(patch.autoFollow);
              if (patch.offset !== undefined) setOffset(patch.offset);
            },
          )
        : null}
    </div>
  );
}

export default LogsAndContainersTab;
