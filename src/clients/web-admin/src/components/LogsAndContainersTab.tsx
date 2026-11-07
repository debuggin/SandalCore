// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import { ServerTimeOfDayControl } from "../map/VerraMapCanvas";
import { DEFAULT_REGIONAL_WORLD_WORKERS, formatWorkerTickHzOptionLabel, normalizeWorkerTickHz, useWorldStreamStore, WORKER_TICK_HZ_OPTIONS, type WorldWorkerInfo } from "../state/worldStreamStore";
import { buildPaginatedQueryUrl, fetchWithTransientRetry, formatLogEntryToString, normalizePaginatedEnvelope, type PaginatedListEnvelope } from "./GmCommandPalette";
import { SANDAL_LAUNCHER_TOKENS } from "./PlayersAndSpawnerTab";

export const CANONICAL_SANDAL_CONTAINERS: readonly string[] = Object.freeze([
  "aoc-world-riverlands", "aoc-postgres", "aoc-auth", "aoc-character", "aoc-citynode", "aoc-quest", "aoc-mesh", "aoc-gateway", "aoc-mesh-gateway", "aoc-web-admin", "aoc-ingress-proxy",
]);
export const REGIONAL_WORKER_CONTAINERS: readonly string[] = Object.freeze([
  "aoc-world-riverlands", "aoc-world-sandsquall", "aoc-world-jundark", "aoc-world-anvils", "aoc-world-turquoise-sea",
]);
export const LOG_CONTAINERS: readonly { readonly name: string; readonly label: string }[] = Object.freeze([
  { name: "aoc-world", label: "aoc-world (Whole-World Authority)" }, { name: "aoc-world-riverlands", label: "aoc-riverlands (Riverlands Region)" },
  { name: "aoc-world-sandsquall", label: "aoc-sandsquall (Sandsquall Region)" }, { name: "aoc-world-jundark", label: "aoc-jundark (Jundark Region)" },
  { name: "aoc-world-anvils", label: "aoc-anvils (Anvils Region)" }, { name: "aoc-world-turquoise-sea", label: "aoc-turquoise-sea (Turquoise Sea Region)" },
]);
export const LOG_TAIL_OPTIONS: readonly number[] = Object.freeze([100, 200, 500]);

export interface PrometheusTargetStatus {
  readonly name: string; readonly url: string; readonly status: string; readonly latency_ms: number; readonly exported_metrics: number; readonly families_count: number;
}
export interface EnvoyClusterHealthRow {
  readonly cluster: string; readonly container: string; readonly requests_total: number; readonly errors_5xx_total: number; readonly timeouts_total: number; readonly active_connections: number; readonly healthy_members: number;
}
export interface PrometheusScrapeReport {
  readonly scraped_at_ms: number; readonly targets: readonly PrometheusTargetStatus[];
  readonly cluster_metrics: Readonly<Record<string, EnvoyClusterHealthRow>>; readonly mesh_gateway_metrics: Readonly<Record<string, number>>;
  readonly world_metrics: {
    readonly connected: boolean; readonly tick_hz: number; readonly tick_p50_ms: number; readonly tick_p99_ms: number;
    readonly publish_ms: number; readonly publish_max_ms: number; readonly subscribers: number;
    readonly npc_count: number; readonly resource_node_count: number; readonly city_node_count: number; readonly active_players: number;
  };
}
export interface ContainerScrapeMetric {
  readonly name: string; readonly image: string; readonly state: string; readonly status: string;
  readonly cpu_pct: number; readonly mem_mb: number; readonly mem_limit_mb: number; readonly mem_pct: number;
  readonly pids: number; readonly rx_bytes: number; readonly tx_bytes: number;
  readonly scrape_target: string; readonly scrape_status: string; readonly scrape_latency_ms: number;
  readonly exported_metrics: number; readonly requests_total: number; readonly errors_total: number; readonly active_connections: number;
}
export interface WorldAnalyticsStatus {
  readonly total_entities: number; readonly players_online: number; readonly npcs_total: number; readonly monsters_total: number;
  readonly resource_nodes_total: number; readonly mounts_total: number; readonly pois_total: number; readonly subscribers: number; readonly data_mode: string;
}
export interface GatewayTelemetrySummary {
  readonly requests_total: number; readonly errors_total: number; readonly cache_hits_total: number; readonly cache_misses_total: number; readonly transient_retries_total: number; readonly uptime_ms: number;
}
export interface WorldAuthorityWorker extends WorldWorkerInfo {
  readonly id: string; readonly name: string; readonly canonical_name: string; readonly worker_index: number;
  readonly region: string; readonly total_entities: number; readonly players: number; readonly resources: number; readonly mounts: number;
  readonly tick_hz: number; readonly tick_p50_ms: number; readonly tick_p99_ms: number;
  readonly admin_addr: string; readonly stream_addr: string; readonly scope_tier_badge: string; readonly dynamic_scaling_stub: boolean;
}
export interface StructuredDockerLogRow {
  readonly index: number; readonly timestamp: string; readonly stream: "stdout" | "stderr"; readonly level: "INFO" | "WARN" | "ERROR" | "DEBUG";
  readonly container: string; readonly component: string; readonly message: string; readonly attrs: ReadonlyArray<readonly [string, string]>; readonly raw: string;
}
export interface GmAuditSummaryRow {
  readonly audit_id: string; readonly actor_role: string; readonly actor_sub?: string; readonly action_name: string;
  readonly target_entity_id: string; readonly diff_summary: string; readonly where_zone?: string; readonly applied_at: string;
}
export interface ContainersScrapeSnapshot {
  readonly world_status: WorldAnalyticsStatus; readonly gateway_telemetry: GatewayTelemetrySummary;
  readonly prometheus: PrometheusScrapeReport; readonly workers: readonly WorldAuthorityWorker[]; readonly containers: readonly ContainerScrapeMetric[];
}

export const DEFAULT_PROMETHEUS_TARGETS: readonly PrometheusTargetStatus[] = Object.freeze([]);
export const EMPTY_GATEWAY_TELEMETRY: GatewayTelemetrySummary = Object.freeze({
  requests_total: 0, errors_total: 0, cache_hits_total: 0, cache_misses_total: 0, transient_retries_total: 0, uptime_ms: 0,
});
export const EMPTY_WORLD_STATUS: WorldAnalyticsStatus = Object.freeze({
  total_entities: 0, players_online: 0, npcs_total: 0, monsters_total: 0, resource_nodes_total: 0, mounts_total: 0, pois_total: 0, subscribers: 0, data_mode: "memory | sqlite-wal",
});

const CLUSTER_DISPLAY_LABELS: Readonly<Record<string, string>> = Object.freeze({
  auth_grpc: "Auth Service (gRPC)", character_grpc: "Character Service (gRPC)", citynode_grpc: "City Node Service (gRPC)",
  quest_grpc: "Quest Service (gRPC)", mesh_grpc: "Mesh Directory (gRPC)", gateway_grpc: "Gateway Service (gRPC)",
  gateway_tls: "Gateway Service (TLS)", gateway_http: "Gateway Service (HTTP)", world_admin_grpc: "World Admin Service (gRPC)",
  world_stream_grpc: "World Stream Service (gRPC)", world_grpc: "World Authority (gRPC)", mesh_gateway_http: "Mesh Gateway (HTTP)",
  mesh_gateway_grpc: "Mesh Gateway (gRPC)", web_admin_http: "Web Admin UI (HTTP)", ingress_downstream: "Ingress Proxy (HTTPS/TLS)",
  ingress_http: "Ingress Proxy (HTTP)",
});

const CLUSTER_PROTOCOL_SUFFIXES: Readonly<Record<string, string>> = Object.freeze({
  grpc: "gRPC", http: "HTTP", https: "HTTPS", tls: "TLS", tcp: "TCP", udp: "UDP", downstream: "HTTPS/TLS",
});

export function formatClusterDisplayName(cluster: string): string {
  const key = (cluster ?? "").trim().toLowerCase().replace(/[-\s]+/g, "_");
  if (CLUSTER_DISPLAY_LABELS[key]) return CLUSTER_DISPLAY_LABELS[key];
  const parts = key.split("_").filter((p) => p.length > 0);
  if (parts.length === 0) return "Service Route (HTTP)";
  const last = parts[parts.length - 1];
  const proto = CLUSTER_PROTOCOL_SUFFIXES[last] ?? "HTTP";
  const stemTokens = CLUSTER_PROTOCOL_SUFFIXES[last] ? parts.slice(0, -1) : parts;
  const stem = stemTokens.map((t) => (t === "citynode" ? "City Node" : t.charAt(0).toUpperCase() + t.slice(1))).join(" ") || "Service";
  const withRole = /Service|Gateway|Proxy|Directory|Authority|UI/i.test(stem) ? stem : `${stem} Service`;
  return `${withRole} (${proto})`;
}

function normalizeContainerEquivKey(raw: string): string {
  const clean = (raw ?? "").trim().toLowerCase().replace(/^aoc-(?:world-)?/, "");
  return clean === "world" ? "riverlands" : clean;
}

export function areContainerNamesEquivalent(a: string, b: string): boolean {
  if (!a || !b) return false;
  return a === b || normalizeContainerEquivKey(a) === normalizeContainerEquivKey(b);
}

function slugifyScopeToken(raw: string): string {
  return raw.trim().toLowerCase().replace(/^(node:|zoi:|poly:|verra_zoi_|world-|aoc-world-|aoc-)/i, "").replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "");
}

export function formatCanonicalWorkerName(scopeKind: string, scopeKey: string, rawName?: string): string {
  const kind = scopeKind.trim().toLowerCase();
  if (kind === "world") return "aoc-world";
  const slug = slugifyScopeToken(scopeKey || rawName || "riverlands") || "riverlands";
  return kind === "polygon_cutout" || kind === "polygon" ? `aoc-poly-${slug}` : `aoc-${slug}`;
}

function defaultTickHzForScope(scopeKind: string): number {
  const kind = scopeKind.trim().toLowerCase();
  return kind === "world" ? 30 : kind === "region" ? 45 : kind === "zoi" || kind === "dynamic_zoi" ? 60 : 100;
}

function formatScopeTierBadge(scopeKind: string, tickHz: number): string {
  const kind = scopeKind.trim().toLowerCase(), hz = Math.round(Number.isFinite(tickHz) && tickHz > 0 ? tickHz : defaultTickHzForScope(kind));
  if (kind === "world") return `WORLD ${hz}Hz`;
  if (kind === "region") return `REGION ${hz}Hz`;
  if (kind.includes("node")) return `NODE_PLUG ${hz}Hz`;
  return kind.includes("poly") ? `POLYGON_CUTOUT ${hz}Hz` : `ZOI ${hz}Hz`;
}

function normalizeSingleWorker(w: Record<string, unknown>, idx: number): WorldAuthorityWorker {
  const rawKind = String(w.scope_kind ?? "region").trim().toLowerCase(), dynamicStub = Boolean(w.dynamic_scaling_stub ?? rawKind.startsWith("dynamic_")), normKind = rawKind.replace(/^dynamic_/, "");
  const scopeKind = normKind === "node" || normKind === "node_plug" || normKind === "node_plug_zoi" ? "node_plug" : normKind === "world" || normKind === "zoi" || normKind === "polygon_cutout" ? normKind : "region";
  const id = String(w.worker_id ?? w.id ?? `worker-${idx}`), scopeKey = String(w.scope_key ?? id), canonicalName = formatCanonicalWorkerName(scopeKind, scopeKey, String(w.name ?? id));
  const tickHz = Number(w.tick_hz ?? defaultTickHzForScope(scopeKind)), npcs = Number(w.npcs ?? w.npc_count ?? 0), monsters = Number(w.monsters ?? w.monster_count ?? 0);
  const resources = Number(w.resources ?? w.resource_nodes ?? w.resource_node_count ?? 0), players = Number(w.players ?? w.active_players ?? w.player_count ?? 0), mounts = Number(w.mounts ?? w.mount_count ?? 0);
  const totalEntities = players + npcs + monsters + resources + mounts, region = String(w.region ?? w.region_label ?? w.scope_label ?? w.label ?? scopeKey);
  const containerName = String(w.container_name ?? w.container ?? (scopeKind === "world" ? "aoc-world" : canonicalName)), rawIndex = Number(w.worker_index ?? w.index);
  return {
    worker_index: Number.isFinite(rawIndex) && rawIndex >= 0 ? Math.trunc(rawIndex) : idx,
    worker_id: id, id, name: String(w.name ?? canonicalName), canonical_name: canonicalName, container_name: containerName,
    scope_kind: scopeKind, scope_key: scopeKey, region, region_label: region, status: String(w.status ?? (w.connected === false ? "OFFLINE" : "UP")).toUpperCase(),
    tick_hz: tickHz, tick_p50_ms: Number(w.tick_p50_ms ?? 0), tick_p99_ms: Number(w.tick_p99_ms ?? 0),
    total_entities: totalEntities, players, active_players: players, player_count: players, npcs, npc_count: npcs, monsters, monster_count: monsters,
    resources, resource_nodes: resources, resource_node_count: resources, mounts, admin_addr: String(w.admin_addr ?? "world:50061"), stream_addr: String(w.stream_addr ?? "world:50062"),
    scope_tier_badge: formatScopeTierBadge(scopeKind, tickHz), dynamic_scaling_stub: dynamicStub,
  };
}

export function normalizeWorldWorkers(rawWorkers: unknown): readonly WorldAuthorityWorker[] {
  if (!Array.isArray(rawWorkers) || rawWorkers.length === 0) return [];
  return (rawWorkers as readonly Record<string, unknown>[])
    .filter((w) => w && typeof w === "object" && String(w.worker_id ?? w.id ?? w.name ?? "").trim().length > 0)
    .map((w, idx) => normalizeSingleWorker(w, idx));
}

function buildEmptyContainerMetric(name: string): ContainerScrapeMetric {
  return { name, image: `sandalcore/${name.replace(/^aoc-/, "")}:latest`, state: "unknown", status: "Awaiting scrape", cpu_pct: 0, mem_mb: 0, mem_limit_mb: 0, mem_pct: 0, pids: 0, rx_bytes: 0, tx_bytes: 0, scrape_target: `${name}:metrics`, scrape_status: "UNKNOWN", scrape_latency_ms: 0, exported_metrics: 0, requests_total: 0, errors_total: 0, active_connections: 0 };
}

function parseClusterMetricsMap(rawClusters: unknown): Readonly<Record<string, EnvoyClusterHealthRow>> {
  const cObj = (rawClusters && typeof rawClusters === "object" ? rawClusters : {}) as Record<string, unknown>;
  const out: Record<string, EnvoyClusterHealthRow> = {};
  for (const [key, val] of Object.entries(cObj)) {
    if (!val || typeof val !== "object") continue;
    const item = val as Record<string, unknown>;
    out[key] = { cluster: String(item.cluster ?? key), container: String(item.container ?? ""), requests_total: Number(item.requests_total ?? 0), errors_5xx_total: Number(item.errors_5xx_total ?? 0), timeouts_total: Number(item.timeouts_total ?? 0), active_connections: Number(item.active_connections ?? 0), healthy_members: Number(item.healthy_members ?? 0) };
  }
  return out;
}

function normalizePrometheusReport(rawProm: unknown, ws: Record<string, unknown>): PrometheusScrapeReport {
  const p = (rawProm && typeof rawProm === "object" ? rawProm : {}) as Record<string, unknown>;
  const rawTargets = Array.isArray(p.targets) ? (p.targets as readonly Record<string, unknown>[]) : [];
  const targets: readonly PrometheusTargetStatus[] = rawTargets.map((t) => ({ name: String(t.name ?? "target"), url: String(t.url ?? ""), status: String(t.status ?? "DOWN").toUpperCase(), latency_ms: Number(t.latency_ms ?? 0), exported_metrics: Number(t.exported_metrics ?? 0), families_count: Number(t.families_count ?? 0) }));
  const mgRaw = (p.mesh_gateway_metrics && typeof p.mesh_gateway_metrics === "object" ? p.mesh_gateway_metrics : {}) as Record<string, unknown>;
  const mesh_gateway_metrics: Record<string, number> = {};
  for (const [k, v] of Object.entries(mgRaw)) mesh_gateway_metrics[k] = Number(v ?? 0);
  const wm = (p.world_metrics && typeof p.world_metrics === "object" ? p.world_metrics : {}) as Record<string, unknown>;
  return {
    scraped_at_ms: Number(p.scraped_at_ms ?? Date.now()), targets, cluster_metrics: parseClusterMetricsMap(p.cluster_metrics), mesh_gateway_metrics,
    world_metrics: { connected: Boolean(wm.connected ?? ws.connected ?? false), tick_hz: Number(wm.tick_hz ?? 0), tick_p50_ms: Number(wm.tick_p50_ms ?? ws.tick_ms ?? 0), tick_p99_ms: Number(wm.tick_p99_ms ?? ws.publish_max_ms ?? 0), publish_ms: Number(wm.publish_ms ?? ws.publish_ms ?? 0), publish_max_ms: Number(wm.publish_max_ms ?? ws.publish_max_ms ?? 0), subscribers: Number(wm.subscribers ?? ws.subscribers ?? 0), npc_count: Number(wm.npc_count ?? ws.npcs_total ?? ws.npcs ?? 0), resource_node_count: Number(wm.resource_node_count ?? ws.resource_nodes_total ?? ws.resource_nodes ?? 0), city_node_count: Number(wm.city_node_count ?? 0), active_players: Number(wm.active_players ?? ws.players_online ?? ws.players ?? 0) },
  };
}

function parseContainerItem(c: Record<string, unknown>): ContainerScrapeMetric {
  const name = String(c.name ?? ""), memMb = Number(c.mem_mb ?? c.memory_mb ?? 0), memLim = Number(c.mem_limit_mb ?? c.memory_limit_mb ?? 0);
  return {
    name, image: String(c.image ?? `sandalcore/${name.replace(/^aoc-/, "")}:latest`), state: String(c.state ?? "unknown"), status: String(c.status ?? "Unknown"),
    cpu_pct: Number(c.cpu_pct ?? c.cpu_percent ?? 0), mem_mb: memMb, mem_limit_mb: memLim, mem_pct: Number(c.mem_pct ?? c.memory_pct ?? (memLim > 0 ? (memMb / memLim) * 100 : 0)),
    pids: Number(c.pids ?? 0), rx_bytes: Number(c.rx_bytes ?? c.net_rx_bytes ?? 0), tx_bytes: Number(c.tx_bytes ?? c.net_tx_bytes ?? 0),
    scrape_target: String(c.scrape_target ?? `${name}:metrics`), scrape_status: String(c.scrape_status ?? (c.state === "running" ? "UP" : "DOWN")).toUpperCase(),
    scrape_latency_ms: Number(c.scrape_latency_ms ?? 0), exported_metrics: Number(c.exported_metrics ?? 0), requests_total: Number(c.requests_total ?? 0), errors_total: Number(c.errors_total ?? 0), active_connections: Number(c.active_connections ?? 0),
  };
}

export function normalizeContainersScrapeResponse(raw: unknown): ContainersScrapeSnapshot {
  const obj = (raw && typeof raw === "object" ? raw : {}) as Record<string, unknown>;
  const ws = (obj.world_status && typeof obj.world_status === "object" ? obj.world_status : obj) as Record<string, unknown>;
  const gt = (obj.gateway_telemetry && typeof obj.gateway_telemetry === "object" ? obj.gateway_telemetry : obj) as Record<string, unknown>;
  const env = normalizePaginatedEnvelope<Record<string, unknown>>(obj, ["containers", "items"], 25, 0);
  const byName = new Map<string, ContainerScrapeMetric>();
  for (const c of env.items) {
    if (!c || typeof c !== "object") continue;
    const parsed = parseContainerItem(c);
    if (parsed.name) byName.set(parsed.name, parsed);
  }
  const containers = Array.from(byName.values());
  const prometheus = normalizePrometheusReport(obj.prometheus, ws);
  const playersOnline = Number(ws.players_online ?? ws.players ?? 0), npcsTotal = Number(ws.npcs_total ?? ws.npcs ?? 0);
  const monstersTotal = Number(ws.monsters_total ?? ws.monsters ?? 0), resourcesTotal = Number(ws.resource_nodes_total ?? ws.resource_nodes ?? 0);
  const mountsTotal = Number(ws.mounts ?? ws.mounts_total ?? 0);
  const totalEntities = Number(ws.total_entities ?? playersOnline + npcsTotal + monstersTotal + resourcesTotal + mountsTotal);
  return {
    world_status: { total_entities: totalEntities, players_online: playersOnline, npcs_total: npcsTotal, monsters_total: monstersTotal, resource_nodes_total: resourcesTotal, mounts_total: mountsTotal, pois_total: Number(ws.pois ?? ws.pois_total ?? 0), subscribers: Number(ws.subscribers ?? prometheus.world_metrics.subscribers ?? 0), data_mode: String(ws.data_mode ?? "memory | sqlite-wal") },
    gateway_telemetry: { requests_total: Number(gt.requests_total ?? 0), errors_total: Number(gt.errors_total ?? 0), cache_hits_total: Number(gt.cache_hits_total ?? 0), cache_misses_total: Number(gt.cache_misses_total ?? 0), transient_retries_total: Number(gt.transient_retries_total ?? 0), uptime_ms: Number(gt.uptime_ms ?? 0) },
    prometheus, workers: normalizeWorldWorkers(obj.workers), containers,
  };
}

function classifyLogSeverity(rawLevel: string, message: string, stream: "stdout" | "stderr"): "INFO" | "WARN" | "ERROR" | "DEBUG" {
  const check = `${rawLevel} ${message}`.toUpperCase();
  if (check.includes("ERROR") || check.includes("FATAL") || check.includes("PANIC") || check.includes("[ERR]")) return "ERROR";
  if (check.includes("WARN") || check.includes("[WRN]")) return "WARN";
  if (check.includes("DEBUG") || check.includes("TRACE") || check.includes("[DBG]")) return "DEBUG";
  return stream === "stderr" && rawLevel.length === 0 ? "WARN" : "INFO";
}

function safeFormatAttrValue(val: unknown): string {
  if (typeof val === "string") return val;
  if (typeof val === "number" || typeof val === "boolean") return String(val);
  if (val === null || val === undefined) return "";
  try { return JSON.stringify(val); } catch { return ""; }
}

function parseSingleDockerLogRow(entry: unknown, index: number, fallbackContainer: string): StructuredDockerLogRow {
  const isObj = entry !== null && typeof entry === "object";
  const obj = (isObj ? entry : {}) as Record<string, unknown>;
  const container = typeof obj.container === "string" && obj.container.trim().length > 0 ? obj.container.trim() : fallbackContainer;
  const stream: "stdout" | "stderr" = obj.stream === "stderr" ? "stderr" : "stdout";
  const rawStr = isObj ? formatLogEntryToString(obj) : String(entry ?? "");
  let timestamp = typeof obj.timestamp === "string" ? obj.timestamp.trim() : "";
  let body = typeof obj.message === "string" && obj.message.trim().length > 0 ? obj.message.trim() : rawStr.trim();
  if (!timestamp && body.length > 20 && body[4] === "-" && body.includes("T")) {
    const sp = body.indexOf(" ");
    if (sp > 18 && sp < 40) {
      timestamp = body.slice(0, sp);
      body = body.slice(sp + 1).trim();
    }
  }
  let levelRaw = typeof obj.level === "string" ? obj.level : "";
  let component = container.replace(/^aoc-/, "");
  const attrs: Array<readonly [string, string]> = [];
  if (body.startsWith("{") && body.endsWith("}")) {
    try {
      const parsed = JSON.parse(body) as Record<string, unknown>;
      timestamp = String(parsed.time ?? parsed.ts ?? parsed.timestamp ?? timestamp);
      levelRaw = String(parsed.level ?? parsed.lvl ?? parsed.severity ?? levelRaw);
      component = String(parsed.component ?? parsed.service ?? parsed.subsystem ?? component);
      body = String(parsed.msg ?? parsed.message ?? body);
      for (const [k, v] of Object.entries(parsed)) {
        if (!["time", "ts", "timestamp", "level", "lvl", "severity", "msg", "message", "component", "service", "subsystem"].includes(k)) {
          const formatted = safeFormatAttrValue(v);
          if (formatted) attrs.push([k, formatted]);
        }
      }
    } catch { /* retain plain body */ }
  }
  const level = classifyLogSeverity(levelRaw, body, stream);
  return { index, timestamp: timestamp || "--:--:--", stream, level, container, component, message: body || rawStr, attrs, raw: rawStr };
}

export function normalizeDockerLogEntries(
  rawEnvelope: unknown,
  fallbackContainer = "aoc-world-riverlands",
): PaginatedListEnvelope<StructuredDockerLogRow> {
  const obj = (rawEnvelope && typeof rawEnvelope === "object" && !Array.isArray(rawEnvelope) ? rawEnvelope : {}) as Record<string, unknown>;
  const container = typeof obj.container === "string" && obj.container.trim().length > 0 ? obj.container.trim() : fallbackContainer;
  const sourceArray: readonly unknown[] = Array.isArray(rawEnvelope)
    ? rawEnvelope
    : Array.isArray(obj.entries) ? obj.entries : Array.isArray(obj.items) ? obj.items : Array.isArray(obj.lines) ? obj.lines : [];
  const rows = sourceArray.map((item, idx) => parseSingleDockerLogRow(item, idx, container));
  const limit = typeof obj.limit === "number" && Number.isFinite(obj.limit) ? obj.limit : Math.max(100, rows.length);
  const offset = typeof obj.offset === "number" && Number.isFinite(obj.offset) ? obj.offset : 0;
  const total = typeof obj.total === "number" && Number.isFinite(obj.total) ? obj.total : offset + rows.length;
  const hasMore = typeof obj.has_more === "boolean" ? obj.has_more : offset + rows.length < total;
  return { items: rows, total, limit, offset, has_more: hasMore, next_cursor: hasMore ? String(offset + limit) : null };
}

function formatByteCount(bytes: number): string {
  const safe = Number(bytes ?? 0);
  if (!Number.isFinite(safe) || safe <= 0) return "0 KB";
  const mb = safe / (1024 * 1024);
  return mb >= 1 ? `${mb.toFixed(1)} MB` : `${(safe / 1024).toFixed(0)} KB`;
}

const PANEL_STYLE: React.CSSProperties = {
  padding: "14px", borderRadius: "12px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
  background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush, backdropFilter: "blur(16px)",
};

const COLLAPSIBLE_PANEL_STYLE: React.CSSProperties = {
  ...PANEL_STYLE, padding: 0, overflow: "hidden",
};

const COLLAPSIBLE_BODY_STYLE: React.CSSProperties = {
  padding: "10px 16px 14px 16px",
};

const SELECT_INPUT_STYLE: React.CSSProperties = {
  minHeight: "32px", padding: "4px 8px", borderRadius: "6px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
  backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "12px",
};

function renderMetricItem(testId: string, label: string, value: string | number, subtext?: string): React.ReactElement {
  return (
    <div key={testId} data-testid={testId} style={{ display: "flex", justifyContent: "space-between", alignItems: "baseline", gap: "8px", padding: "3px 0", borderBottom: "1px solid rgba(140, 108, 66, 0.16)" }}>
      <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, fontSize: "11px" }}>{label}</span>
      <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "13px", fontWeight: 700, fontVariantNumeric: "tabular-nums" }}>
        {value}
        {subtext ? <span style={{ marginLeft: "6px", fontSize: "11px", fontWeight: 500, color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400 }}>{subtext}</span> : null}
      </span>
    </div>
  );
}

const PILL_STYLE: React.CSSProperties = {
  minWidth: "112px", height: "22px", padding: "0 8px", display: "inline-flex", alignItems: "center", justifyContent: "center",
  borderRadius: "999px", fontSize: "11px", fontWeight: 600, fontVariantNumeric: "tabular-nums",
  border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush, color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400,
};

function renderCollapsibleHeader(title: string, pills: readonly string[], collapsed: boolean, onToggle: () => void, testId?: string): React.ReactElement {
  return (
    <div role="button" tabIndex={0} aria-expanded={!collapsed} data-testid={testId} onClick={onToggle} onKeyDown={(e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); onToggle(); } }} style={{ display: "flex", flexWrap: "wrap", alignItems: "center", justifyContent: "space-between", gap: "8px", width: "100%", minHeight: "48px", padding: "12px 16px", margin: 0, boxSizing: "border-box", cursor: "pointer", userSelect: "none", borderBottom: collapsed ? "none" : "1px solid rgba(140, 108, 66, 0.22)" }}>
      <div style={{ display: "flex", alignItems: "center", gap: "8px", flexWrap: "wrap" }}>
        <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500, fontSize: "12px", fontWeight: 700 }}>{collapsed ? "\u25B8" : "\u25BE"}</span>
        <strong style={{ fontSize: "13px", letterSpacing: "0.06em", color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 }}>{title}</strong>
        {pills.length > 0 ? <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, fontSize: "11px" }}>{"->"}</span> : null}
        {pills.map((p, i) => (<span key={i} style={PILL_STYLE}>{p}</span>))}
      </div>
      <span style={{ fontSize: "11px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>{collapsed ? "Expand" : "Collapse"}</span>
    </div>
  );
}

function renderTelemetrySummaryHeader(snap: ContainersScrapeSnapshot): React.ReactElement {
  const { world_status: ws, gateway_telemetry: gt, prometheus: prom, containers } = snap;
  const storeSnap = useWorldStreamStore.getState();
  const totalSeries = prom.targets.reduce((acc, t) => acc + Number(t.exported_metrics ?? 0), 0);
  const totalFamilies = prom.targets.reduce((acc, t) => acc + Number(t.families_count ?? 0), 0);
  const udpFrames = Number(prom.mesh_gateway_metrics.udp_frames_rx_total ?? 0) + Number(prom.mesh_gateway_metrics.udp_frames_tx_total ?? 0) + Number(prom.mesh_gateway_metrics.aoc_mesh_udp_packets_total ?? 0);
  const hitTotal = gt.cache_hits_total + gt.cache_misses_total, hitPct = hitTotal > 0 ? `${Math.round((gt.cache_hits_total / hitTotal) * 100)}%` : "100%";
  const runningCount = containers.filter((c) => c.state === "running").length, uptimeSec = Math.max(0, Math.round(gt.uptime_ms / 1000));
  const titleStyle: React.CSSProperties = { display: "block", marginBottom: "6px", fontSize: "11px", letterSpacing: "0.08em", textTransform: "uppercase", color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 };
  return (
    <div data-testid="telemetry-summary-grid" style={{ display: "grid", gridTemplateColumns: "repeat(4, 1fr)", gap: "10px" }}>
      <div data-testid="golden-card-simulation-latency" style={{ ...PANEL_STYLE, position: "relative", zIndex: 40 }}>
        <strong style={titleStyle}>Simulation</strong>
        {renderMetricItem("metric-world-tick-hz", "Tick Hz (p50 / p99)", `${Number(prom.world_metrics.tick_hz ?? 30).toFixed(1)} Hz`, `${Number(prom.world_metrics.tick_p50_ms ?? 0).toFixed(1)} / ${Number(prom.world_metrics.tick_p99_ms ?? 0).toFixed(1)} ms`)}
        {renderMetricItem("metric-stream-publish-ms", "Publish Avg / Max", `${Number(prom.world_metrics.publish_ms ?? 0).toFixed(2)} / ${Number(prom.world_metrics.publish_max_ms ?? 0).toFixed(2)} ms`, `Subs: ${ws.subscribers} | 30 Hz`)}
        <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", gap: "6px", paddingTop: "4px", flexWrap: "wrap" }}>
          <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, fontSize: "12px" }}>Server Clock (9x)</span>
          <ServerTimeOfDayControl storeSnap={storeSnap} compact />
        </div>
      </div>
      <div data-testid="golden-card-world-population" style={PANEL_STYLE}>
        <strong style={titleStyle}>World Population</strong>
        {renderMetricItem("metric-total-entities", "Total Entities", ws.total_entities, `${prom.world_metrics.city_node_count} CityNodes | POIs: ${ws.pois_total}`)}
        {renderMetricItem("metric-players-online", "Players", ws.players_online, `Data: ${ws.data_mode}`)}
        {renderMetricItem("metric-npcs-alive", "NPCs", ws.npcs_total)}
        {renderMetricItem("metric-monsters-alive", "Monsters", ws.monsters_total)}
        {renderMetricItem("metric-resources-active", "Resource Nodes", ws.resource_nodes_total)}
        {renderMetricItem("metric-mounts-active", "Mounts", ws.mounts_total)}
      </div>
      <div data-testid="golden-card-gateway-traffic" style={PANEL_STYLE}>
        <strong style={titleStyle}>Traffic & Cache</strong>
        {renderMetricItem("metric-requests-total", "Gateway Requests", gt.requests_total, `Up: ${uptimeSec}s`)}
        {renderMetricItem("metric-cache-hits", "Cache Hits / Misses", `${gt.cache_hits_total} / ${gt.cache_misses_total}`, `(${hitPct})`)}
        {renderMetricItem("metric-udp-frames", "UDP Frames Forwarded", udpFrames)}
      </div>
      <div data-testid="golden-card-error-saturation" style={PANEL_STYLE}>
        <strong style={titleStyle}>Errors & Saturation</strong>
        {renderMetricItem("metric-errors-total", "Gateway Errors / Retries", gt.errors_total, `Retries: ${gt.transient_retries_total}`)}
        {renderMetricItem("metric-active-containers", "Active Containers", `${runningCount} / ${containers.length}`)}
        {renderMetricItem("metric-prom-series", "OTel Families / Series", `${totalFamilies} / ${totalSeries}`)}
      </div>
    </div>
  );
}

function renderWorldAuthorityWorkersCard(
  workers: readonly WorldAuthorityWorker[],
  selectedContainer: string,
  onSelectContainer: (name: string) => void,
  collapsed = false,
  onToggle: () => void = () => {},
): React.ReactElement {
  const storeSnap = useWorldStreamStore.getState();
  const headers = ["Mesh #", "Canonical Worker", "Scope Tier & Hz", "Scope Key / ZOI", "gRPC Admin / Stream", "Tick p50 / p99", "Total Entities", "Players", "NPCs", "Monsters", "Resources", "Status", "Logs"];
  const pills = [`${workers.filter((w) => w.status === "UP" || w.status === "ONLINE").length}/${workers.length} Online`, `${workers.reduce((s, w) => s + w.total_entities, 0)} Entities`, `${workers.reduce((s, w) => s + w.players, 0)} Players`];
  return (
    <section aria-label="World Shards" data-testid="world-authority-workers-card" style={COLLAPSIBLE_PANEL_STYLE}>
      {renderCollapsibleHeader("World Shards", pills, collapsed, onToggle, "toggle-card-world-shards")}
      {collapsed ? null : (
        <div style={COLLAPSIBLE_BODY_STYLE}>
          <table style={{ width: "100%", borderCollapse: "collapse", fontSize: "12px", fontVariantNumeric: "tabular-nums" }}>
            <thead>
              <tr style={{ textAlign: "left", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, borderBottom: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}` }}>
                {headers.map((h) => (<th key={h} style={{ padding: "6px" }}>{h}</th>))}
              </tr>
            </thead>
            <tbody>
              {workers.map((w) => {
                const isUp = w.status === "UP" || w.status === "ONLINE";
                const isSel = areContainerNamesEquivalent(w.container_name, selectedContainer) || areContainerNamesEquivalent(w.canonical_name, selectedContainer);
                const liveWorker = storeSnap.workers.find((sw) => sw.worker_id === w.worker_id);
                const curHz = normalizeWorkerTickHz(liveWorker?.tick_hz ?? w.tick_hz, liveWorker?.tick_mode ?? w.tick_mode);
                return (
                  <tr key={w.worker_id} data-testid={`worker-row-${w.worker_id}`} onClick={() => onSelectContainer(w.container_name)} style={{ cursor: "pointer", backgroundColor: isSel ? "rgba(232, 178, 90, 0.14)" : "transparent", borderBottom: "1px solid rgba(140, 108, 66, 0.22)" }}>
                    <td style={{ padding: "7px 6px", fontWeight: 700, color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400 }}>#{w.worker_index}</td>
                    <td style={{ padding: "7px 6px", fontWeight: 700, color: isSel ? SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100 }}>{w.canonical_name} <span style={{ fontSize: "11px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>({w.worker_id})</span></td>
                    <td style={{ padding: "7px 6px", display: "flex", alignItems: "center", gap: "6px", flexWrap: "wrap" }}>
                      <span style={{ padding: "2px 7px", borderRadius: "999px", fontSize: "11px", fontWeight: 700, backgroundColor: "rgba(232, 178, 90, 0.16)", color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400 }}>{formatScopeTierBadge(w.scope_kind, curHz)}</span>
                      <select aria-label={`Tick rate for ${w.worker_id}`} data-testid={`analytics-worker-tick-select-${w.worker_id}`} value={curHz} onClick={(e) => e.stopPropagation()} onChange={(e) => { e.stopPropagation(); void useWorldStreamStore.getState().executeSetWorkerTickRateAction(w.worker_id, Number(e.currentTarget.value)); }} style={{ height: "22px", padding: "0 6px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush, color: SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500, fontSize: "11px", fontWeight: 700, cursor: "pointer" }}>
                        {WORKER_TICK_HZ_OPTIONS.map((hz) => (<option key={hz} value={hz}>{formatWorkerTickHzOptionLabel(hz, false)}</option>))}
                      </select>
                      {w.dynamic_scaling_stub ? <span style={{ padding: "2px 6px", borderRadius: "999px", fontSize: "10px", backgroundColor: "rgba(32, 216, 206, 0.14)", color: SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500 }}>Dynamic Scale Stub (min=1)</span> : null}
                    </td>
                    <td style={{ padding: "7px 6px", color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100 }}>{w.region_label} ({w.scope_key})</td>
                    <td style={{ padding: "7px 6px", fontSize: "11px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>{w.admin_addr} | {w.stream_addr}</td>
                    <td style={{ padding: "7px 6px" }}>{w.tick_p50_ms.toFixed(1)} / {w.tick_p99_ms.toFixed(1)} ms</td>
                    <td style={{ padding: "7px 6px", fontWeight: 700, color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 }}>{w.total_entities}</td>
                    <td style={{ padding: "7px 6px" }}>{w.players}</td>
                    <td style={{ padding: "7px 6px" }}>{w.npcs}</td>
                    <td style={{ padding: "7px 6px" }}>{w.monsters}</td>
                    <td style={{ padding: "7px 6px" }}>{w.resources}</td>
                    <td style={{ padding: "7px 6px" }}><span style={{ padding: "2px 8px", borderRadius: "999px", fontSize: "11px", fontWeight: 700, backgroundColor: isUp ? "rgba(32, 216, 206, 0.16)" : "rgba(224, 82, 88, 0.18)", color: isUp ? SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500 : SANDAL_LAUNCHER_TOKENS.VerraRuinCrimson500 }}>{w.status}</span></td>
                    <td style={{ padding: "7px 6px" }}><button type="button" data-testid={`worker-logs-btn-${w.worker_id}`} onClick={(e) => { e.stopPropagation(); onSelectContainer(w.container_name); }} style={{ minHeight: "26px", padding: "2px 8px", borderRadius: "6px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: isSel ? SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: isSel ? SANDAL_LAUNCHER_TOKENS.VerraObsidian100 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "11px", fontWeight: 600, cursor: "pointer" }}>Logs</button></td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}
    </section>
  );
}

const DEFAULT_CLUSTER_ROUTES: readonly EnvoyClusterHealthRow[] = Object.freeze([]);

function renderServiceRoutingCard(prom: PrometheusScrapeReport, collapsed = false, onToggle: () => void = () => {}): React.ReactElement {
  const clusters = Object.values(prom.cluster_metrics);
  const pills = [`${clusters.length} Routes`, `Req ${clusters.reduce((s, c) => s + Math.round(c.requests_total), 0)}`, `5xx ${clusters.reduce((s, c) => s + Math.round(c.errors_5xx_total), 0)}`];
  return (
    <section aria-label="Service Routing & Connection Health" data-testid="service-routing-card" style={COLLAPSIBLE_PANEL_STYLE}>
      {renderCollapsibleHeader("Service Routing & Connection Health", pills, collapsed, onToggle, "toggle-card-service-routing")}
      {collapsed ? null : (
        <div data-testid="envoy-cluster-red-strip" style={{ ...COLLAPSIBLE_BODY_STYLE, display: "grid", gridTemplateColumns: "repeat(auto-fill, minmax(300px, 1fr))", gap: "8px" }}>
          {clusters.map((cl) => (
            <div key={cl.cluster} data-testid={`envoy-cluster-${cl.cluster}`} style={{ padding: "8px 10px", borderRadius: "6px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush, fontSize: "11px", fontVariantNumeric: "tabular-nums" }}>
              <div style={{ display: "flex", justifyContent: "space-between", marginBottom: "4px" }}>
                <strong style={{ color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400 }}>{formatClusterDisplayName(cl.cluster)}</strong>
                <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>{cl.cluster}</span>
              </div>
              <div style={{ color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100 }}>
                Requests (req={Math.round(cl.requests_total)}) | 5xx Errors (5xx={Math.round(cl.errors_5xx_total)}) | Timeouts (timeout={Math.round(cl.timeouts_total)}) | Active Conn (cx={Math.round(cl.active_connections)}) | Healthy (healthy={Math.round(cl.healthy_members)})
              </div>
            </div>
          ))}
        </div>
      )}
    </section>
  );
}

function renderPrometheusTargetsCard(prom: PrometheusScrapeReport, collapsed = false, onToggle: () => void = () => {}): React.ReactElement {
  const pills = [`${prom.targets.filter((t) => t.status === "UP").length}/${prom.targets.length} Targets UP`, `${prom.targets.reduce((s, t) => s + Number(t.families_count ?? 0), 0)} Families`, `${prom.targets.reduce((s, t) => s + Number(t.exported_metrics ?? 0), 0)} Series`];
  return (
    <section aria-label="Telemetry & Metrics" data-testid="prometheus-scrape-card" style={COLLAPSIBLE_PANEL_STYLE}>
      {renderCollapsibleHeader("Telemetry & Metrics", pills, collapsed, onToggle, "toggle-card-telemetry-metrics")}
      {collapsed ? null : (
        <div style={{ ...COLLAPSIBLE_BODY_STYLE, display: "grid", gridTemplateColumns: "repeat(3, 1fr)", gap: "10px" }}>
          {prom.targets.map((t) => {
            const isUp = t.status === "UP";
            return (
              <div key={t.name} data-testid={`prom-target-${t.name}`} style={{ padding: "8px 10px", borderRadius: "8px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush, display: "flex", justifyContent: "space-between", alignItems: "center", gap: "8px" }}>
                <div>
                  <div style={{ fontWeight: 700, fontSize: "12px", color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100 }}>{t.name} ({t.url})</div>
                  <div style={{ fontSize: "11px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>{Number(t.families_count ?? 0)} families | {Number(t.exported_metrics ?? 0)} series | {Number(t.latency_ms ?? 0).toFixed(1)} ms</div>
                </div>
                <span style={{ padding: "2px 8px", borderRadius: "999px", fontSize: "11px", fontWeight: 700, backgroundColor: isUp ? "rgba(32, 216, 206, 0.16)" : "rgba(224, 82, 88, 0.18)", color: isUp ? SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500 : SANDAL_LAUNCHER_TOKENS.VerraRuinCrimson500 }}>{t.status}</span>
              </div>
            );
          })}
        </div>
      )}
    </section>
  );
}

function renderContainerTableRow(c: ContainerScrapeMetric, selectedContainer: string, onSelectContainer: (name: string) => void): React.ReactElement {
  const isHealthy = c.state === "running", isScrapeUp = c.scrape_status === "UP", isSelected = areContainerNamesEquivalent(c.name, selectedContainer);
  return (
    <tr key={c.name} data-testid={`container-row-${c.name}`} onClick={() => onSelectContainer(c.name)} style={{ cursor: "pointer", backgroundColor: isSelected ? "rgba(232, 178, 90, 0.14)" : "transparent", borderBottom: "1px solid rgba(140, 108, 66, 0.22)" }}>
      <td style={{ padding: "7px 6px", fontWeight: 600, color: isSelected ? SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100 }}>{c.name}</td>
      <td style={{ padding: "7px 6px", fontSize: "11px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>{c.image}</td>
      <td style={{ padding: "7px 6px" }}><span style={{ padding: "2px 8px", borderRadius: "999px", backgroundColor: isHealthy ? "rgba(32, 216, 206, 0.16)" : "rgba(184, 196, 212, 0.16)", color: isHealthy ? SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500 : SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, fontSize: "11px" }}>{c.state} ({c.status})</span></td>
      <td style={{ padding: "7px 6px" }}>{Number(c.cpu_pct ?? 0).toFixed(2)}%</td>
      <td style={{ padding: "7px 6px" }}>{Number(c.mem_mb ?? 0).toFixed(1)} / {Math.round(Number(c.mem_limit_mb ?? 0))} MB ({Number(c.mem_pct ?? 0).toFixed(1)}%)</td>
      <td style={{ padding: "7px 6px" }}>{Math.round(Number(c.pids ?? 0))}</td>
      <td style={{ padding: "7px 6px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>{formatByteCount(c.rx_bytes)} / {formatByteCount(c.tx_bytes)}</td>
      <td style={{ padding: "7px 6px" }}>
        <span style={{ color: isScrapeUp ? SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500 : SANDAL_LAUNCHER_TOKENS.VerraRuinCrimson500, fontWeight: 700, fontSize: "11px" }}>{c.scrape_status || "UP"}</span>
        <span style={{ marginLeft: "6px", fontSize: "11px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>({Math.round(Number(c.exported_metrics ?? 0))} series | {Number(c.scrape_latency_ms ?? 0).toFixed(1)} ms)</span>
      </td>
      <td style={{ padding: "7px 6px" }}><button type="button" data-testid={`tail-logs-btn-${c.name}`} onClick={(e) => { e.stopPropagation(); onSelectContainer(c.name); }} style={{ minHeight: "26px", padding: "2px 8px", borderRadius: "6px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: isSelected ? SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: isSelected ? SANDAL_LAUNCHER_TOKENS.VerraObsidian100 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "11px", fontWeight: 600, cursor: "pointer" }}>Logs</button></td>
    </tr>
  );
}

function renderContainersScrapeTable(
  containers: readonly ContainerScrapeMetric[],
  selectedContainer: string,
  onSelectContainer: (name: string) => void,
  collapsed = false,
  onToggle: () => void = () => {},
): React.ReactElement {
  const headers = ["Container", "Image", "State / Status", "CPU %", "Memory (Used / Limit / %)", "PIDs", "Net RX / TX", "OTel Scrape (Status / Series / Latency)", "Logs"];
  const pills = [`${containers.filter((c) => c.state === "running").length}/${containers.length} Running`, `CPU ${containers.reduce((s, c) => s + Number(c.cpu_pct ?? 0), 0).toFixed(1)}%`, `RAM ${Math.round(containers.reduce((s, c) => s + Number(c.mem_mb ?? 0), 0))} MB`];
  return (
    <section aria-label="Container Metrics & Analytics" data-testid="containers-scrape-card" style={COLLAPSIBLE_PANEL_STYLE}>
      {renderCollapsibleHeader("Container Metrics & Analytics", pills, collapsed, onToggle, "toggle-card-container-metrics")}
      {collapsed ? null : (
        <div style={COLLAPSIBLE_BODY_STYLE}>
          <table style={{ width: "100%", borderCollapse: "collapse", fontSize: "12px", fontVariantNumeric: "tabular-nums" }}>
            <thead>
              <tr style={{ textAlign: "left", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, borderBottom: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}` }}>
                {headers.map((h) => (<th key={h} style={{ padding: "6px" }}>{h}</th>))}
              </tr>
            </thead>
            <tbody>
              {containers.map((c) => renderContainerTableRow(c, selectedContainer, onSelectContainer))}
            </tbody>
          </table>
        </div>
      )}
    </section>
  );
}

function renderStructuredLogRowItem(row: StructuredDockerLogRow): React.ReactElement {
  const levelColor = row.level === "ERROR" ? SANDAL_LAUNCHER_TOKENS.VerraRuinCrimson500 : row.level === "WARN" ? SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 : row.level === "DEBUG" ? SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 : SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500;
  return (
    <div key={row.index} data-testid={`docker-log-row-${row.index}`} style={{ display: "flex", flexWrap: "wrap", alignItems: "baseline", gap: "8px", padding: "3px 4px", borderBottom: "1px solid rgba(140, 108, 66, 0.14)", fontFamily: "ui-monospace, SFMono-Regular, Consolas, monospace", fontSize: "11px" }}>
      <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, fontVariantNumeric: "tabular-nums" }}>{row.timestamp}</span>
      <span style={{ padding: "1px 6px", borderRadius: "4px", fontWeight: 700, fontSize: "10px", color: levelColor, border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}` }}>{row.level}</span>
      <span style={{ padding: "1px 6px", borderRadius: "4px", fontSize: "10px", color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400 }}>{row.stream}</span>
      <span style={{ padding: "1px 6px", borderRadius: "4px", fontSize: "10px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>[{row.component}]</span>
      <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, wordBreak: "break-word", flex: 1 }}>{row.message}</span>
      {row.attrs.map(([k, v]) => (<span key={k} style={{ padding: "1px 5px", borderRadius: "4px", backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400, fontSize: "10px" }}>{k}={v}</span>))}
    </div>
  );
}

export const AUTO_TAIL_TOOLTIP = "Auto Tail: Continuously poll the selected container log stream and update the log view with new entries.";
export const AUTO_FOLLOW_TOOLTIP = "Auto Follow: Keep the most recent log entry visible by following the tail. Scrolling up turns off Auto Follow; scrolling to the bottom re-enables Auto Follow.";

export function computeNextAutoFollowOnScroll(scrollTop: number, scrollHeight: number, clientHeight: number, currentAutoFollow: boolean): boolean {
  const atBottom = scrollHeight - scrollTop - clientHeight <= 16;
  if (!atBottom && currentAutoFollow) return false;
  if (atBottom && !currentAutoFollow) return true;
  return currentAutoFollow;
}

interface DockerLogTailerProps {
  readonly selectedContainer: string; readonly selectableContainers: readonly string[];
  readonly tailCount: number; readonly autoTail: boolean; readonly autoFollow: boolean;
  readonly logFilter: string; readonly levelFilter: string; readonly streamFilter: string;
  readonly logEnv: PaginatedListEnvelope<StructuredDockerLogRow>;
  readonly logViewportRef: React.RefObject<HTMLDivElement | null>;
  readonly isProgrammaticScrollRef: React.MutableRefObject<boolean>;
}

function renderDockerLogTailerCard(
  props: DockerLogTailerProps,
  onControlsChange: (patch: { container?: string; tail?: number; autoTail?: boolean; autoFollow?: boolean; q?: string; level?: string; stream?: string; offset?: number }) => void,
  collapsed = false,
  onToggle: () => void = () => {},
): React.ReactElement {
  const filteredRows = props.logEnv.items.filter((r) => (props.levelFilter === "ALL" || r.level === props.levelFilter) && (props.streamFilter === "ALL" || r.stream === props.streamFilter));
  const title = `Log Explorer (${props.selectedContainer}) (${filteredRows.length} lines)`;
  const pills = [props.selectedContainer, `${filteredRows.length} / ${props.logEnv.total} Lines`, `Tail: ${props.autoTail ? "ON" : "OFF"} · Follow: ${props.autoFollow ? "ON" : "OFF"}`];
  return (
    <section aria-label={title} data-testid="docker-log-tailer-card" style={COLLAPSIBLE_PANEL_STYLE}>
      {renderCollapsibleHeader(title, pills, collapsed, onToggle, "toggle-card-log-explorer")}
      {collapsed ? null : (
        <div style={COLLAPSIBLE_BODY_STYLE}>
          <div style={{ display: "flex", gap: "6px", alignItems: "center", flexWrap: "wrap", marginBottom: "10px" }}>
            <select aria-label="Container selector" data-testid="log-container-select" value={props.selectedContainer} onChange={(e) => onControlsChange({ container: e.currentTarget.value, offset: 0 })} style={SELECT_INPUT_STYLE}>
              {props.selectableContainers.map((name) => (<option key={name} value={name}>{name}</option>))}
            </select>
            <select aria-label="Severity filter" data-testid="log-level-filter" value={props.levelFilter} onChange={(e) => onControlsChange({ level: e.currentTarget.value })} style={SELECT_INPUT_STYLE}>
              {["ALL", "ERROR", "WARN", "INFO", "DEBUG"].map((lv) => (<option key={lv} value={lv}>Level: {lv}</option>))}
            </select>
            <select aria-label="Stream filter" data-testid="log-stream-filter" value={props.streamFilter} onChange={(e) => onControlsChange({ stream: e.currentTarget.value, offset: 0 })} style={SELECT_INPUT_STYLE}>
              {["ALL", "stdout", "stderr"].map((st) => (<option key={st} value={st}>Stream: {st}</option>))}
            </select>
            <select aria-label="Log tail count" data-testid="log-tail-select" value={props.tailCount} onChange={(e) => onControlsChange({ tail: Number(e.currentTarget.value), offset: 0 })} style={SELECT_INPUT_STYLE}>
              {LOG_TAIL_OPTIONS.map((t) => (<option key={t} value={t}>Tail {t}</option>))}
            </select>
            <label title={AUTO_TAIL_TOOLTIP} style={{ ...SELECT_INPUT_STYLE, display: "inline-flex", alignItems: "center", gap: "5px", padding: "4px 10px", cursor: "pointer" }}>
              <input type="checkbox" data-testid="logs-auto-tail-toggle" title={AUTO_TAIL_TOOLTIP} checked={props.autoTail} onChange={(e) => onControlsChange({ autoTail: e.currentTarget.checked })} />
              <span>Auto Tail</span>
            </label>
            <label title={AUTO_FOLLOW_TOOLTIP} style={{ ...SELECT_INPUT_STYLE, display: "inline-flex", alignItems: "center", gap: "5px", padding: "4px 10px", cursor: "pointer" }}>
              <input type="checkbox" data-testid="logs-auto-follow-toggle" title={AUTO_FOLLOW_TOOLTIP} checked={props.autoFollow} onChange={(e) => onControlsChange({ autoFollow: e.currentTarget.checked })} />
              <span>Auto Follow</span>
            </label>
            <input type="search" aria-label="Filter container logs" data-testid="log-filter-input" value={props.logFilter} onChange={(e) => onControlsChange({ q: e.currentTarget.value, offset: 0 })} placeholder="Filter log lines..." style={SELECT_INPUT_STYLE} />
          </div>
          <div
            ref={props.logViewportRef as React.LegacyRef<HTMLDivElement>}
            data-testid="logs-scroll-viewport"
            onScroll={(e) => {
              if (props.isProgrammaticScrollRef.current) return;
              const el = e.currentTarget;
              const nextFollow = computeNextAutoFollowOnScroll(el.scrollTop, el.scrollHeight, el.clientHeight, props.autoFollow);
              if (nextFollow !== props.autoFollow) onControlsChange({ autoFollow: nextFollow });
            }}
            style={{ margin: 0, padding: "10px", maxHeight: "360px", overflowY: "auto", borderRadius: "8px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: "rgba(6, 8, 14, 0.94)", color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100 }}
          >
            <div data-testid="docker-log-output">
              {filteredRows.length > 0 ? filteredRows.map((row) => renderStructuredLogRowItem(row)) : "No log lines received yet."}
            </div>
          </div>
        </div>
      )}
    </section>
  );
}

function renderGmAuditLogCard(auditRows: readonly GmAuditSummaryRow[], collapsed = false, onToggle: () => void = () => {}): React.ReactElement {
  const pills = [`${auditRows.length} Entries`];
  return (
    <section aria-label="GM Audit Log" data-testid="gm-audit-trail-strip" style={COLLAPSIBLE_PANEL_STYLE}>
      {renderCollapsibleHeader("GM Audit Log", pills, collapsed, onToggle, "toggle-card-gm-audit")}
      {collapsed ? null : (
        <div style={COLLAPSIBLE_BODY_STYLE}>
          <div data-testid="gm-audit-log-body" style={{ margin: 0, padding: "10px", maxHeight: "180px", overflowY: "auto", borderRadius: "8px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: "rgba(6, 8, 14, 0.94)", display: "flex", flexDirection: "column", gap: "6px", fontSize: "11px" }}>
            {auditRows.length > 0 ? auditRows.map((a, idx) => (
              <div key={a.audit_id || idx} data-testid={`gm-audit-row-${idx}`} style={{ display: "grid", gridTemplateColumns: "150px 1fr 160px 160px", gap: "8px", alignItems: "center", padding: "4px 6px", borderBottom: "1px solid rgba(140, 108, 66, 0.14)" }}>
                <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400, fontWeight: 700 }}>Who: {a.actor_sub ? `${a.actor_sub} (${a.actor_role})` : a.actor_role || "GM_ADMIN"}</span>
                <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100 }}>What: <strong>{a.action_name}</strong> {a.target_entity_id} {a.diff_summary ? `(${a.diff_summary})` : ""}</span>
                <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500 }}>Where: {a.where_zone || a.target_entity_id || "verra"}</span>
                <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, fontVariantNumeric: "tabular-nums" }}>When: {a.applied_at || "--:--:--"}</span>
              </div>
            )) : <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>No GM audit actions recorded yet.</span>}
          </div>
        </div>
      )}
    </section>
  );
}

export interface LogsAndContainersTabProps {
  readonly mode?: "analytics" | "logs" | "both";
}

function useContainersScrapePolling(mode: "analytics" | "logs" | "both", onWorkersUpdate: (workers: readonly WorldWorkerInfo[]) => void): ContainersScrapeSnapshot {
  const [scrapeSnap, setScrapeSnap] = React.useState<ContainersScrapeSnapshot>(() => normalizeContainersScrapeResponse({}));
  const [, setClockTick] = React.useState(0);
  React.useEffect(() => {
    if (typeof window === "undefined" || typeof fetch !== "function" || mode === "logs") return;
    let cancelled = false;
    const pollContainers = (): void => {
      fetchWithTransientRetry("/api/v1/gm/containers").then((r) => (r.ok ? r.json() : null)).then((raw) => {
        if (cancelled || !raw) return;
        const next = normalizeContainersScrapeResponse(raw);
        setScrapeSnap(next);
        onWorkersUpdate(next.workers);
      }).catch(() => {});
    };
    pollContainers();
    const timer = window.setInterval(pollContainers, 3000);
    const clockTimer = window.setInterval(() => setClockTick((n) => n + 1), 1000);
    return () => { cancelled = true; window.clearInterval(timer); window.clearInterval(clockTimer); };
  }, [mode]);
  return scrapeSnap;
}

function useContainerLogsPolling(
  mode: "analytics" | "logs" | "both",
  selectedContainer: string,
  logQuery: string,
  tail: number,
  autoFollow: boolean,
): { readonly logEnv: PaginatedListEnvelope<StructuredDockerLogRow>; readonly auditRows: readonly GmAuditSummaryRow[]; readonly setOffset: (n: number) => void } {
  const [offset, setOffset] = React.useState(0);
  const [logEnv, setLogEnv] = React.useState<PaginatedListEnvelope<StructuredDockerLogRow>>(() => normalizeDockerLogEntries({}, selectedContainer));
  const [auditRows, setAuditRows] = React.useState<readonly GmAuditSummaryRow[]>([]);
  React.useEffect(() => {
    if (typeof window === "undefined" || typeof fetch !== "function" || mode === "analytics") return;
    let cancelled = false;
    const pollLogs = (): void => {
      const url = buildPaginatedQueryUrl("/api/v1/gm/logs", { q: logQuery, limit: tail, offset, extra: { container: selectedContainer, tail } });
      fetchWithTransientRetry(url).then((r) => (r.ok ? r.json() : null)).then((raw) => { if (!cancelled && raw) setLogEnv(normalizeDockerLogEntries(raw, selectedContainer)); }).catch(() => {});
      fetchWithTransientRetry("/api/v1/gm/audit?limit=25").then((r) => (r.ok ? r.json() : null)).then((raw) => {
        if (!cancelled && raw) setAuditRows(normalizePaginatedEnvelope<GmAuditSummaryRow>(raw, ["entries", "items"], 25, 0).items);
      }).catch(() => {});
    };
    pollLogs();
    if (!autoFollow) return () => { cancelled = true; };
    const timer = window.setInterval(pollLogs, 2000);
    return () => { cancelled = true; window.clearInterval(timer); };
  }, [mode, selectedContainer, tail, autoFollow, logQuery, offset]);
  return { logEnv, auditRows, setOffset };
}

export function LogsAndContainersTab(props?: LogsAndContainersTabProps): React.ReactElement {
  const mode = props?.mode ?? "both";
  const storeSnap = useWorldStreamStore();
  const tail = storeSnap.logTailLines ?? 100;
  const autoTail = storeSnap.logAutoTail ?? true;
  const autoFollow = storeSnap.logAutoFollow ?? true;
  const levelFilter = storeSnap.logLevelFilter ?? "ALL";
  const streamFilter = storeSnap.logStreamFilter ?? "ALL";
  const collapsed = storeSnap.collapsedSections ?? {};
  const logViewportRef = React.useRef<HTMLDivElement | null>(null);
  const isProgrammaticScrollRef = React.useRef(false);
  const toggleSection = (key: string): void => storeSnap.patchSessionUi({ collapsedSections: { ...collapsed, [key]: !collapsed[key] } });
  const scrapeSnap = useContainersScrapePolling(mode, storeSnap.setWorkers);
  const { logEnv, auditRows, setOffset } = useContainerLogsPolling(mode, storeSnap.selectedContainer, storeSnap.logQuery, tail, autoTail);
  React.useEffect(() => {
    if (!autoFollow || !logViewportRef.current) return;
    isProgrammaticScrollRef.current = true;
    logViewportRef.current.scrollTop = logViewportRef.current.scrollHeight;
    isProgrammaticScrollRef.current = false;
  }, [autoFollow, logEnv.items.length]);
  const showAnalytics = mode === "analytics" || mode === "both", showLogs = mode === "logs" || mode === "both";
  const selectContainer = (container: string): void => { storeSnap.setSelectedContainer(container); setOffset(0); };
  const selectableContainers = Array.from(new Set([...scrapeSnap.containers.map((c) => c.name), ...scrapeSnap.workers.map((w) => w.container_name), storeSnap.selectedContainer])).filter((name) => name.length > 0);
  return (
    <div aria-label="Observability" data-testid="logs-and-containers-tab" style={{ display: "flex", flexDirection: "column", gap: "12px", padding: "16px", backgroundColor: SANDAL_LAUNCHER_TOKENS.VerraObsidian100, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, minHeight: "540px" }}>
      {showAnalytics ? renderTelemetrySummaryHeader(scrapeSnap) : null}
      {showAnalytics ? renderWorldAuthorityWorkersCard(scrapeSnap.workers, storeSnap.selectedContainer, selectContainer, Boolean(collapsed.shards), () => toggleSection("shards")) : null}
      {showAnalytics ? renderContainersScrapeTable(scrapeSnap.containers, storeSnap.selectedContainer, selectContainer, Boolean(collapsed.containers), () => toggleSection("containers")) : null}
      {showAnalytics ? renderServiceRoutingCard(scrapeSnap.prometheus, Boolean(collapsed.routing), () => toggleSection("routing")) : null}
      {showAnalytics ? renderPrometheusTargetsCard(scrapeSnap.prometheus, Boolean(collapsed.endpoints), () => toggleSection("endpoints")) : null}
      {showLogs
        ? renderDockerLogTailerCard(
            { selectedContainer: storeSnap.selectedContainer, selectableContainers, tailCount: tail, autoTail, autoFollow, logFilter: storeSnap.logQuery, levelFilter, streamFilter, logEnv, logViewportRef, isProgrammaticScrollRef },
            (patch) => {
              if (patch.container !== undefined) storeSnap.setSelectedContainer(patch.container);
              if (patch.q !== undefined) storeSnap.setLogQuery(patch.q);
              if (patch.level !== undefined) storeSnap.patchSessionUi({ logLevelFilter: patch.level });
              if (patch.stream !== undefined) storeSnap.patchSessionUi({ logStreamFilter: patch.stream });
              if (patch.tail !== undefined) storeSnap.patchSessionUi({ logTailLines: patch.tail });
              if (patch.autoTail !== undefined) storeSnap.patchSessionUi({ logAutoTail: patch.autoTail });
              if (patch.autoFollow !== undefined) storeSnap.patchSessionUi({ logAutoFollow: patch.autoFollow });
              if (patch.offset !== undefined) setOffset(patch.offset);
            },
            Boolean(collapsed.logs),
            () => toggleSection("logs"),
          )
        : null}
      {showLogs ? renderGmAuditLogCard(auditRows, Boolean(collapsed.audit), () => toggleSection("audit")) : null}
    </div>
  );
}

export default LogsAndContainersTab;
