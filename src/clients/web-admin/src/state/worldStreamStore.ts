// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import { VERRA_ZOI_POLYGONS, type VerraZoiPolygon } from "../map/verraZoiPolygons";
import { getActiveAdminJwtToken, isUnauthenticatedViewOnly, resetAllRetryTimersAndCircuits, subscribeRetryReset } from "./adminAuthClient";
import {
  DEFAULT_SESSION_UI_EXTRA_STATE, DEFAULT_VERRA_CITY_NODES, expandViewportWorkerCatalog, formatTimeOfDayLabel, getCityNodeCenterCm, mosaicPxToWorldCm,
  nodeLevelToPlugTier, parseTimeOfDayInput, type PoiOverlay, resolveTabSessionStorage, SESSION_UI_STORAGE_KEY, type SessionUiExtraState,
  setSubpartitionWorkerTickHz, STREAM_TARGET_HZ, UI_FRAME_BUDGET_MS, VERRA_MAP_SOURCE_CM, VERRA_MAP_SOURCE_PX, type VerraCityNodeMeta,
  type VerraUnrealPoint, WEB_ADMIN_STORAGE_KEY, worldCmToMosaicPx,
} from "./verraSpatialMath";

export * from "./adminAuthClient";
export * from "./verraSpatialMath";

export type WorkerScopeKind = "world" | "region" | "zoi" | "node_plug" | "node" | "node_plug_zoi" | "dynamic_node_plug_zoi" | "dynamic_zoi" | "polygon_cutout";

export interface WorldWorkerInfo {
  readonly worker_id: string; readonly worker_index?: number; readonly canonical_name?: string;
  readonly scope_key: string; readonly scope_kind: WorkerScopeKind | string; readonly region_label: string;
  readonly container_name: string; readonly polygon_cutout?: ReadonlyArray<readonly [number, number]>;
  readonly dynamic_scaling_stub?: boolean; readonly admin_addr?: string; readonly stream_addr?: string;
  readonly status: string; readonly npc_count: number; readonly monster_count?: number;
  readonly resource_node_count: number; readonly player_count: number; readonly npcs: number;
  readonly monsters: number; readonly resource_nodes: number; readonly active_players: number;
  readonly tick_hz?: number; readonly tick_p50_ms?: number; readonly tick_p99_ms?: number;
}

export type WorldWorkerRecord = WorldWorkerInfo;

export interface ResolvedWorkerAuthority {
  readonly worker_id: string; readonly worker_index?: number; readonly canonical_name?: string;
  readonly scope_key: string; readonly scope_kind: WorkerScopeKind | string;
  readonly container_name: string; readonly region_label: string;
}

export const DYNAMIC_WORKER_NATO_SUFFIXES = [
  "alpha", "beta", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india", "juliett", "kilo", "lima", "mike",
  "november", "oscar", "papa", "quebec", "romeo", "sierra", "tango", "uniform", "victor", "whiskey", "xray", "yankee", "zulu",
] as const;

export const MAX_DYNAMIC_WORKERS_PER_SCOPE = 26;

function slugifyWorkerKey(raw: string): string {
  return raw.trim().replace(/([a-z0-9])([A-Z])/g, "$1-$2").toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "");
}

export function formatCanonicalWorkerName(
  scopeKind: string,
  scopeKey: string,
  options?: { readonly dynamic?: boolean; readonly dynamicOrdinal?: number },
): string {
  const kind = scopeKind.trim().toLowerCase();
  if (kind === "world") return "aoc-world";
  if (kind === "polygon_cutout") return `aoc-poly-${slugifyWorkerKey(scopeKey) || "cutout"}`;
  const isNode = kind === "node_plug" || kind === "node" || kind === "node_plug_zoi" || kind === "dynamic_node_plug_zoi";
  const stripped = isNode ? scopeKey.replace(/^Verra_[A-Za-z0-9]+_(?:Node_)?/i, "") : scopeKey;
  const base = `aoc-${slugifyWorkerKey(stripped || scopeKey) || (isNode ? "node" : "world")}`;
  const isDynamic = options?.dynamic === true || kind === "dynamic_zoi" || kind === "dynamic_node_plug_zoi";
  if (!isDynamic) return base;
  const ord = Math.max(0, Math.min(MAX_DYNAMIC_WORKERS_PER_SCOPE - 1, Math.trunc(options?.dynamicOrdinal ?? 0)));
  return `${base}-${DYNAMIC_WORKER_NATO_SUFFIXES[ord]}`;
}

const REGIONAL_WORKER_TABLE: Readonly<Record<string, ResolvedWorkerAuthority>> = Object.freeze({
  riverlands: { worker_id: "world-riverlands", worker_index: 0, canonical_name: "aoc-riverlands", scope_key: "riverlands", scope_kind: "region", container_name: "aoc-world-riverlands", region_label: "Riverlands" },
  sandsquall: { worker_id: "world-sandsquall", worker_index: 1, canonical_name: "aoc-sandsquall", scope_key: "sandsquall", scope_kind: "region", container_name: "aoc-world-sandsquall", region_label: "Sandsquall" },
  jundark: { worker_id: "world-jundark", worker_index: 2, canonical_name: "aoc-jundark", scope_key: "jundark", scope_kind: "region", container_name: "aoc-world-jundark", region_label: "Jundark" },
  anvils: { worker_id: "world-anvils", worker_index: 3, canonical_name: "aoc-anvils", scope_key: "anvils", scope_kind: "region", container_name: "aoc-world-anvils", region_label: "Anvils" },
  turquoise_sea: { worker_id: "world-turquoise-sea", worker_index: 4, canonical_name: "aoc-turquoise-sea", scope_key: "turquoise_sea", scope_kind: "region", container_name: "aoc-world-turquoise-sea", region_label: "Turquoise Sea" },
});

export const DEFAULT_REGIONAL_WORLD_WORKERS: readonly WorldWorkerInfo[] = Object.freeze(
  Object.values(REGIONAL_WORKER_TABLE).map((w, idx) => ({
    worker_id: w.worker_id, worker_index: w.worker_index ?? idx, canonical_name: w.canonical_name ?? formatCanonicalWorkerName(w.scope_kind, w.scope_key),
    scope_key: w.scope_key, scope_kind: w.scope_kind, region_label: w.region_label, container_name: w.container_name, dynamic_scaling_stub: false,
    status: "UP", npc_count: 0, monster_count: 0, resource_node_count: 0, player_count: 0, npcs: 0, monsters: 0, resource_nodes: 0, active_players: 0,
    tick_hz: 30.0, tick_p50_ms: 0.5, tick_p99_ms: 2.0,
  })),
);

function normalizeRegionKey(raw: string): string | null {
  const s = raw.trim().toLowerCase();
  if (!s) return null;
  if (s.includes("sandsquall") || s.includes("_san_")) return "sandsquall";
  if (s.includes("jundark") || s.includes("_jun_") || s.includes("jungle")) return "jundark";
  if (s.includes("anvil") || s.includes("_anv_") || s.includes("underrealm")) return "anvils";
  if (s.includes("turquoise") || s.includes("_tur_") || s.includes("tropic")) return "turquoise_sea";
  if (s.includes("riverland") || s.includes("_rvr_")) return "riverlands";
  return null;
}

function isPointInPolygon2D(px: number, py: number, ring: readonly (readonly [number, number])[]): boolean {
  let inside = false;
  for (let i = 0, j = ring.length - 1; i < ring.length; j = i++) {
    const [xi, yi] = ring[i];
    const [xj, yj] = ring[j];
    if ((yi > py) !== (yj > py) && px < ((xj - xi) * (py - yi)) / (yj - yi + 1e-12) + xi) inside = !inside;
  }
  return inside;
}

export function findZoiContainingWorldCm(xCm: number, yCm: number): VerraZoiPolygon | null {
  if (!Number.isFinite(xCm) || !Number.isFinite(yCm)) return null;
  const [u, v] = worldCmToMosaicPx(xCm, yCm);
  for (const zoi of VERRA_ZOI_POLYGONS) {
    if (!zoi.name.includes("Lawless_Ocean") && zoi.rings.some((ring) => ring.length >= 3 && isPointInPolygon2D(u, v, ring))) return zoi;
  }
  let bestZoi: VerraZoiPolygon | null = null;
  let bestDistSq = Number.POSITIVE_INFINITY;
  for (const zoi of VERRA_ZOI_POLYGONS) {
    if (zoi.name.includes("Lawless_Ocean")) continue;
    const dSq = (u - zoi.centroid_u) * (u - zoi.centroid_u) + (v - zoi.centroid_v) * (v - zoi.centroid_v);
    if (dSq < bestDistSq) { bestDistSq = dSq; bestZoi = zoi; }
  }
  return bestZoi;
}

function workerRecordToAuthority(w: WorldWorkerInfo): ResolvedWorkerAuthority {
  return {
    worker_id: w.worker_id, worker_index: w.worker_index,
    canonical_name: w.canonical_name ?? formatCanonicalWorkerName(w.scope_kind, w.scope_key),
    scope_key: w.scope_key, scope_kind: w.scope_kind,
    container_name: w.container_name || formatCanonicalWorkerName(w.scope_kind, w.scope_key),
    region_label: w.region_label || w.scope_key,
  };
}

function matchDynamicActiveWorker(
  ctx: { readonly xCm: number; readonly yCm: number; readonly zoiMatch: VerraZoiPolygon | null; readonly nodeMatchName: string; readonly regKey: string },
  activeWorkers: readonly WorldWorkerInfo[],
): ResolvedWorkerAuthority | null {
  if (activeWorkers.length === 0) return null;
  const [u, v] = worldCmToMosaicPx(ctx.xCm, ctx.yCm);
  for (const w of activeWorkers) {
    if (w.scope_kind === "polygon_cutout" && w.polygon_cutout && w.polygon_cutout.length >= 3) {
      if (isPointInPolygon2D(ctx.xCm, ctx.yCm, w.polygon_cutout) || isPointInPolygon2D(u, v, w.polygon_cutout)) return workerRecordToAuthority(w);
    }
  }
  if (ctx.nodeMatchName) {
    const nSlug = slugifyWorkerKey(ctx.nodeMatchName.replace(/^Verra_[A-Za-z0-9]+_(?:Node_)?/i, ""));
    const nw = activeWorkers.find((w) => (w.scope_kind === "node_plug" || w.scope_kind === "node") && (slugifyWorkerKey(w.scope_key).includes(nSlug) || slugifyWorkerKey(w.worker_id).includes(nSlug)));
    if (nw) return workerRecordToAuthority(nw);
  }
  if (ctx.zoiMatch) {
    const zSlug = slugifyWorkerKey(ctx.zoiMatch.display_name || ctx.zoiMatch.name);
    const zw = activeWorkers.find((w) => (w.scope_kind === "zoi" || w.scope_kind === "dynamic_zoi") && (w.scope_key === ctx.zoiMatch?.name || slugifyWorkerKey(w.scope_key).includes(zSlug) || slugifyWorkerKey(w.worker_id).includes(zSlug)));
    if (zw) return workerRecordToAuthority(zw);
  }
  const rw = activeWorkers.find((w) => w.scope_kind === "region" && (normalizeRegionKey(w.scope_key) === ctx.regKey || normalizeRegionKey(w.worker_id) === ctx.regKey));
  if (rw) return workerRecordToAuthority(rw);
  const ww = activeWorkers.find((w) => w.scope_kind === "world" || w.worker_id === "aoc-world" || w.canonical_name === "aoc-world");
  return ww ? workerRecordToAuthority(ww) : null;
}

export function resolveWorkerForCoordinateOrZone(
  x_cm: number,
  y_cm: number,
  zone?: string,
  activeWorkersOrZoi?: readonly WorldWorkerInfo[] | string,
  nodeName?: string,
): ResolvedWorkerAuthority {
  const cleanZone = (zone ?? "").trim();
  const zoiHint = typeof activeWorkersOrZoi === "string" ? activeWorkersOrZoi : "";
  const activeWorkers = Array.isArray(activeWorkersOrZoi) ? activeWorkersOrZoi : [];
  if (cleanZone.toLowerCase().startsWith("zoi:")) {
    const key = cleanZone.slice(4).trim();
    const slug = slugifyWorkerKey(key);
    return { worker_id: `world-zoi-${slug}`, canonical_name: formatCanonicalWorkerName("zoi", key), scope_key: key, scope_kind: "zoi", container_name: `aoc-world-zoi-${slug}`, region_label: key };
  }
  if (cleanZone.toLowerCase().startsWith("node:")) {
    const key = cleanZone.slice(5).trim();
    const slug = slugifyWorkerKey(key);
    return { worker_id: `world-node-${slug}`, canonical_name: formatCanonicalWorkerName("node_plug", key), scope_key: key, scope_kind: "node", container_name: `aoc-world-node-${slug}`, region_label: key };
  }
  const zoiByHint = [cleanZone, zoiHint, nodeName ?? ""].map((h) => VERRA_ZOI_POLYGONS.find((z) => z.name === h || z.display_name === h)).find(Boolean) ?? null;
  const zoiMatch = zoiByHint ?? (Number.isFinite(x_cm) && Number.isFinite(y_cm) ? findZoiContainingWorldCm(x_cm, y_cm) : null);
  const nearNode = DEFAULT_VERRA_CITY_NODES.find((n) => {
    if (nodeName && (n.name === nodeName || n.display_name === nodeName)) return true;
    const { cx_cm, cy_cm } = getCityNodeCenterCm(n);
    return Math.hypot(x_cm - cx_cm, y_cm - cy_cm) <= 50000;
  });
  const regKey = [cleanZone, zoiHint, nodeName ?? "", zoiMatch?.region ?? ""].map(normalizeRegionKey).find(Boolean) ?? "riverlands";
  const dynMatch = matchDynamicActiveWorker({ xCm: x_cm, yCm: y_cm, zoiMatch, nodeMatchName: nearNode?.display_name ?? nodeName ?? "", regKey }, activeWorkers);
  return dynMatch ?? REGIONAL_WORKER_TABLE[regKey] ?? REGIONAL_WORKER_TABLE.riverlands;
}

export type AdminTabId = "map" | "analytics" | "logs" | "players-spawner" | "logs-containers";
export type GmModalId = "give-item" | "give-currency" | "teleport" | "spawn-npc" | "node-governance" | null;
export type TeleportSubMode = "place" | "player" | "to_player" | "coords";
export type SpawnTargetSubMode = "player" | "place" | "coords";
export type NodeElectionAction = "start" | "open_voting" | "conclude" | "cancel";
export type NodeElectionStatus = "none" | "registration" | "voting" | "active_term";
export type NpcInstanceAction = "kill_npc" | "heal_npc" | "reset_npc" | "respawn_npc" | "despawn_npc";

export type CommandReceiptScope =
  | "nodeLevel" | "nodeMayor" | "nodeElection" | "nodeXp"
  | "currency" | "giveItem" | "teleport" | "moveSpeed" | "spawnNpc" | "entityAction" | "worker_tick" | "timeOfDay";

export const WORKER_TICK_HZ_OPTIONS: readonly number[] = Object.freeze([5, 10, 15, 20, 30, 60, 100, 120]);

export interface CommandReceipt {
  readonly scope: CommandReceiptScope; readonly ok: boolean; readonly message: string;
  readonly timestampMs: number; readonly correlationId?: string;
}

export interface GmActionToast {
  readonly id: string; readonly text: string; readonly tone: "success" | "error" | "info"; readonly createdAtMs: number;
}

export interface WorkerSpatialArea {
  readonly minX: number; readonly minY: number; readonly maxX: number; readonly maxY: number;
  readonly centerXCm: number; readonly centerYCm: number; readonly targetZoom: number;
}

const REGIONAL_BOUNDS_CM: Readonly<Record<string, WorkerSpatialArea>> = Object.freeze({
  riverlands: { minX: -1150000, minY: 250000, maxX: -420000, maxY: 780000, centerXCm: -777664, centerYCm: 522023, targetZoom: 3.5 },
  sandsquall: { minX: -650000, minY: 680000, maxX: 120000, maxY: 1180000, centerXCm: -220997, centerYCm: 923599, targetZoom: 3.5 },
  jundark: { minX: -1820000, minY: 820000, maxX: -720000, maxY: 1250000, centerXCm: -1332265, centerYCm: 1048353, targetZoom: 3.5 },
  anvils: { minX: -1480000, minY: -600000, maxX: -720000, maxY: -100000, centerXCm: -1069784, centerYCm: -341306, targetZoom: 3.5 },
  turquoise_sea: { minX: -2100000, minY: -350000, maxX: -1350000, maxY: 560000, centerXCm: -1764281, centerYCm: 114557, targetZoom: 3.5 },
});

export function resolveWorkerSpatialArea(worker: WorldWorkerInfo): WorkerSpatialArea {
  const kind = (worker.scope_kind || "").toLowerCase();
  const keySlug = slugifyWorkerKey(worker.scope_key || worker.worker_id);
  if (kind === "polygon_cutout" && worker.polygon_cutout && worker.polygon_cutout.length >= 3) {
    const pts = worker.polygon_cutout.map(([a, b]) => (Math.abs(a) <= 26000 && Math.abs(b) <= 26000 ? mosaicPxToWorldCm(a, b) : ([a, b] as const)));
    const xs = pts.map((p) => p[0]), ys = pts.map((p) => p[1]);
    const minX = Math.min(...xs), maxX = Math.max(...xs), minY = Math.min(...ys), maxY = Math.max(...ys);
    return { minX, minY, maxX, maxY, centerXCm: (minX + maxX) * 0.5, centerYCm: (minY + maxY) * 0.5, targetZoom: 4.5 };
  }
  const node = DEFAULT_VERRA_CITY_NODES.find((n) => keySlug.includes(slugifyWorkerKey(n.display_name)) || keySlug.includes(slugifyWorkerKey(n.name)));
  if (kind === "node_plug" || kind === "node" || kind === "node_plug_zoi" || kind === "dynamic_node_plug_zoi") {
    if (node) {
      const { cx_cm, cy_cm } = getCityNodeCenterCm(node);
      return { minX: cx_cm - 180000, minY: cy_cm - 180000, maxX: cx_cm + 180000, maxY: cy_cm + 180000, centerXCm: cx_cm, centerYCm: cy_cm, targetZoom: 5 };
    }
  }
  if (kind === "zoi" || kind === "dynamic_zoi") {
    const zoi = VERRA_ZOI_POLYGONS.find((z) => z.name === worker.scope_key || keySlug.includes(slugifyWorkerKey(z.display_name)) || keySlug.includes(slugifyWorkerKey(z.name)));
    if (zoi) {
      const [cx, cy] = mosaicPxToWorldCm(zoi.centroid_u, zoi.centroid_v);
      return { minX: cx - 220000, minY: cy - 220000, maxX: cx + 220000, maxY: cy + 220000, centerXCm: cx, centerYCm: cy, targetZoom: 4.5 };
    }
    if (node) {
      const { cx_cm, cy_cm } = getCityNodeCenterCm(node);
      return { minX: cx_cm - 220000, minY: cy_cm - 220000, maxX: cx_cm + 220000, maxY: cy_cm + 220000, centerXCm: cx_cm, centerYCm: cy_cm, targetZoom: 4.5 };
    }
  }
  if (kind === "region") {
    const regKey = normalizeRegionKey(worker.scope_key || worker.worker_id) ?? "riverlands";
    return REGIONAL_BOUNDS_CM[regKey] ?? REGIONAL_BOUNDS_CM.riverlands;
  }
  return { minX: -2375000, minY: -875000, maxX: 185000, maxY: 1685000, centerXCm: -777664, centerYCm: 522023, targetZoom: 1 };
}

export function getVisibleWorkersForViewport(
  workers: readonly WorldWorkerInfo[],
  viewportBoundsCm?: ViewportWorldBoundsCm | null,
): readonly WorldWorkerInfo[] {
  const list = expandViewportWorkerCatalog(workers.length > 0 ? workers : DEFAULT_REGIONAL_WORLD_WORKERS);
  if (!viewportBoundsCm) return list;
  const visible = list.filter((w) => {
    const a = resolveWorkerSpatialArea(w);
    return a.maxX >= viewportBoundsCm.minX && a.minX <= viewportBoundsCm.maxX && a.maxY >= viewportBoundsCm.minY && a.minY <= viewportBoundsCm.maxY;
  });
  return visible.length > 0 ? visible : list;
}

export const DEFAULT_RECEIPTS_BY_SCOPE: Readonly<Record<CommandReceiptScope, CommandReceipt | null>> = Object.freeze({
  nodeLevel: null, nodeMayor: null, nodeElection: null, nodeXp: null, currency: null,
  giveItem: null, teleport: null, moveSpeed: null, spawnNpc: null, entityAction: null, worker_tick: null, timeOfDay: null,
});

export type WorldEntityKind = "player" | "npc" | "monster" | "poi" | "spawn" | "resource_node" | "mount" | "loot_pile";

export const STREAM_LAYER_PLAYERS = 1;
export const STREAM_LAYER_NPCS = 2;
export const STREAM_LAYER_RESOURCE_NODES = 4;
export const STREAM_LAYER_MOUNTS = 8;
export const STREAM_LAYER_LOOT_PILES = 16;
export const STREAM_LAYER_ALL = 31;
export const LAYER_POIS = 32;
export const LAYER_MONSTERS = 64;

export const OVERLAY_LAYER_KEYS = Object.freeze([
  "players", "npcs", "monsters", "resource_nodes", "mounts", "loot_piles", "pois",
] as const);

export type OverlayLayerKey = (typeof OVERLAY_LAYER_KEYS)[number];

export const OVERLAY_LAYER_BITMASK: Readonly<Record<OverlayLayerKey, number>> = Object.freeze({
  players: STREAM_LAYER_PLAYERS, npcs: STREAM_LAYER_NPCS, monsters: LAYER_MONSTERS,
  resource_nodes: STREAM_LAYER_RESOURCE_NODES, mounts: STREAM_LAYER_MOUNTS, loot_piles: STREAM_LAYER_LOOT_PILES, pois: LAYER_POIS,
});

export interface WorldEntity {
  readonly guid: string; readonly name: string; readonly kind: WorldEntityKind;
  readonly xCm: number; readonly yCm: number; readonly zCm: number; readonly yawDeg: number;
  readonly level: number; readonly healthPct: number; readonly npc_subkind?: string;
  readonly resourceType?: string; readonly quality?: string;
  readonly inCombat?: boolean; readonly dead?: boolean; readonly mounted?: boolean; readonly zoneId?: string;
}

export interface LayerToggles {
  readonly players: boolean; readonly npcs: boolean; readonly monsters: boolean; readonly resource_nodes: boolean;
  readonly mounts: boolean; readonly loot_piles: boolean; readonly pois: boolean; readonly cityNodes: boolean;
  readonly nodePlugs: boolean; readonly zoiBorders: boolean; readonly spawns: boolean; readonly navmesh: boolean;
}

export interface LayerEntityCounts {
  readonly players: number; readonly npcs: number; readonly monsters: number; readonly resource_nodes: number;
  readonly resources: number; readonly mounts: number; readonly loot_piles: number; readonly pois: number;
}

export type CameraMode = "2d" | "3d";

export interface CameraViewport {
  readonly longitude: number; readonly latitude: number; readonly zoom: number;
  readonly pitch: number; readonly bearing: number; readonly mode: CameraMode;
  readonly centerXCm: number; readonly centerYCm: number;
}

export type StreamConnectionState = "connecting" | "live" | "degraded" | "offline" | "replaying";

export interface StreamProblemDetails {
  readonly status: number; readonly title: string; readonly detail: string;
  readonly correlationId?: string; readonly retryAfterSeconds?: number;
}

export interface WorldDeltaBatch {
  readonly seq: number; readonly tick?: number; readonly tickLatencyMs?: number; readonly isSnapshot?: boolean;
  readonly added?: readonly WorldEntity[]; readonly updated?: readonly WorldEntity[]; readonly removed?: readonly string[];
}

export interface NavmeshLinkSegment {
  readonly segmentId: string;
  readonly fromCm: readonly [number, number, number];
  readonly toCm: readonly [number, number, number];
}

export interface PersistedWebAdminState extends Partial<SessionUiExtraState> {
  readonly activeTab: AdminTabId; readonly activeModal: GmModalId;
  readonly camera: Pick<CameraViewport, "centerXCm" | "centerYCm" | "zoom" | "mode">;
  readonly layers: LayerToggles; readonly layerMask: number; readonly selectedPlayer: string;
  readonly selectedEntityGuid?: string | null;
  readonly selectedCityNodeName: string | null; readonly lastClickedPoint: VerraUnrealPoint | null;
  readonly teleportMode: TeleportSubMode; readonly spawnTargetMode: SpawnTargetSubMode;
  readonly selectedNpcTemplate: string; readonly selectedContainer: string; readonly logQuery: string;
  readonly simulateAllNpcs?: boolean;
}

export interface WorldStreamState extends SessionUiExtraState {
  readonly entities: ReadonlyMap<string, WorldEntity>; readonly cityNodes: readonly VerraCityNodeMeta[];
  readonly pois: readonly PoiOverlay[]; readonly workers: readonly WorldWorkerInfo[]; readonly toasts: readonly GmActionToast[];
  readonly selectedEntityGuid: string | null; readonly selectedCityNodeName: string | null; readonly selectedPoiGuid: string | null;
  readonly selectedPlayer: string; readonly activeTab: AdminTabId; readonly activeModal: GmModalId;
  readonly teleportMode: TeleportSubMode; readonly spawnTargetMode: SpawnTargetSubMode;
  readonly selectedNpcTemplate: string; readonly selectedContainer: string; readonly logQuery: string;
  readonly simulateAllNpcs: boolean; readonly layers: LayerToggles; readonly camera: CameraViewport;
  readonly connectionState: StreamConnectionState; readonly lastSeq: number; readonly tickLatencyMs: number;
  readonly streamHz: number; readonly needsSnapshotResync: boolean; readonly error: StreamProblemDetails | null;
  readonly pickedCoordinate: VerraUnrealPoint | null; readonly lastClickedPoint: VerraUnrealPoint | null;
  readonly navmeshSegments: readonly NavmeshLinkSegment[];
  readonly lastReceiptsByScope: Readonly<Record<CommandReceiptScope, CommandReceipt | null>>;
  readonly pushToast: (text: string, tone?: "success" | "error" | "info") => GmActionToast;
  readonly dismissToast: (id: string) => void; readonly applyWorldDelta: (delta: WorldDeltaBatch) => void;
  readonly selectEntity: (guid: string | null) => void; readonly selectCityNode: (name: string | null) => void;
  readonly selectPoi: (guid: string | null) => void; readonly clearMapSelection: () => void;
  readonly setSelectedPlayer: (player: string) => void; readonly setActiveTab: (tab: AdminTabId) => void;
  readonly setActiveModal: (modal: GmModalId) => void; readonly openCommandModal: (modal: GmModalId | "spawn", point?: VerraUnrealPoint) => void; readonly setTeleportMode: (mode: TeleportSubMode) => void;
  readonly setSpawnTargetMode: (mode: SpawnTargetSubMode) => void; readonly setSelectedNpcTemplate: (npc: string) => void;
  readonly setSelectedContainer: (container: string) => void; readonly setLogQuery: (query: string) => void;
  readonly patchSessionUi: (patch: Partial<SessionUiExtraState>) => void;
  readonly saveSessionUiState: (storage?: Pick<Storage, "setItem"> | null) => PersistedWebAdminState;
  readonly restoreForAuthenticatedSession: (storage?: Pick<Storage, "getItem"> | null) => void;
  readonly setSimulateAllNpcs: (enabled?: boolean, fetchImpl?: typeof fetch) => Promise<boolean>;
  readonly setCityNodes: (nodes: readonly VerraCityNodeMeta[]) => void; readonly setPois: (pois: readonly PoiOverlay[]) => void;
  readonly setWorkers: (workers: readonly WorldWorkerInfo[]) => void; readonly updateCityNodeLevel: (name: string, level: number) => void;
  readonly setScopedReceipt: (
    scope: CommandReceiptScope,
    receiptOrMessage: (Omit<CommandReceipt, "scope" | "timestampMs"> & { readonly timestampMs?: number }) | string | null,
    tone?: "success" | "error" | "info",
  ) => void;
  readonly clearScopedReceipt: (scope: CommandReceiptScope) => void;
  readonly executeSetWorkerTickRateAction: (workerId: string, tickHz: number, fetchImpl?: typeof fetch) => Promise<void>;
  readonly executeSetTimeOfDayAction: (hour: number | string, fetchImpl?: typeof fetch) => Promise<void>;
  readonly setNodeMayor: (nodeName: string, mayorCharacterGuidOrName: string, fetchImpl?: typeof fetch) => Promise<CommandReceipt>;
  readonly triggerNodeElection: (nodeName: string, action: NodeElectionAction, fetchImpl?: typeof fetch) => Promise<CommandReceipt>;
  readonly toggleLayer: (layer: keyof LayerToggles, enabled?: boolean) => void; readonly resetLayers: () => void;
  readonly setCameraViewport: (patch: Partial<CameraViewport>) => void; readonly setCameraMode: (mode: CameraMode) => void;
  readonly setConnectionState: (connectionState: StreamConnectionState, error?: StreamProblemDetails | null) => void;
  readonly setPickedCoordinate: (point: VerraUnrealPoint | null) => void; readonly setSpatialOverlays: (navmesh: readonly NavmeshLinkSegment[]) => void;
  readonly resetForUnauthenticatedSession: () => void;
}

export interface WorldStreamStoreApi {
  readonly getState: () => WorldStreamState; readonly getInitialState: () => WorldStreamState;
  readonly setState: (partial: Partial<WorldStreamState> | ((state: WorldStreamState) => Partial<WorldStreamState>)) => void;
  readonly subscribe: (listener: (state: WorldStreamState, prevState: WorldStreamState) => void) => () => void;
}

export interface UseWorldStreamStoreHook extends WorldStreamStoreApi {
  <U = WorldStreamState>(selector?: (state: WorldStreamState) => U): U;
}

export const DEFAULT_LAYER_TOGGLES: LayerToggles = Object.freeze({
  players: true, npcs: true, monsters: true, resource_nodes: true, mounts: true, loot_piles: true,
  pois: true, cityNodes: true, nodePlugs: true, zoiBorders: true, spawns: true, navmesh: false,
});

export function computeStreamLayerMask(layers: Partial<Pick<LayerToggles, OverlayLayerKey>>): number {
  let mask = 0;
  for (const key of OVERLAY_LAYER_KEYS) { if (layers[key] === true) mask |= OVERLAY_LAYER_BITMASK[key]; }
  return mask;
}

export function buildActiveLayersParam(layers: Partial<Pick<LayerToggles, OverlayLayerKey>>): string {
  return OVERLAY_LAYER_KEYS.filter((key) => layers[key] === true).join(",");
}

export function shouldPollWorldState(layers: Partial<Pick<LayerToggles, OverlayLayerKey>>): boolean {
  return computeStreamLayerMask(layers) > 0;
}

export const ZOOM_UPDATE_HZ_TIERS: readonly number[] = [30, 30, 30, 30, 30, 30, 30, 30];

export function getMapPollHzForZoom(_zoom: number): number {
  return 30;
}

export function getMapPollIntervalMsForZoom(_zoom: number): number {
  return 33;
}

export interface ViewportWorldBoundsCm {
  readonly minX: number; readonly minY: number; readonly maxX: number; readonly maxY: number;
}

export function computeViewportWorldBoundsCm(
  camera: Pick<CameraViewport, "centerXCm" | "centerYCm" | "zoom">,
  canvasWidth = typeof window !== "undefined" && window.innerWidth > 0 ? window.innerWidth : 1920,
  canvasHeight = typeof window !== "undefined" && window.innerHeight > 0 ? window.innerHeight : 1080,
): ViewportWorldBoundsCm {
  const scale = Math.pow(2, (Number.isFinite(camera.zoom) ? camera.zoom : 3) - 5);
  const cmPerScreenPx = VERRA_MAP_SOURCE_CM / VERRA_MAP_SOURCE_PX / Math.max(1e-6, scale);
  const halfW = Math.max(25000, Math.max(320, canvasWidth) * cmPerScreenPx * 0.65);
  const halfH = Math.max(25000, Math.max(240, canvasHeight) * cmPerScreenPx * 0.65);
  return { minX: Math.round(camera.centerXCm - halfW), minY: Math.round(camera.centerYCm - halfH), maxX: Math.round(camera.centerXCm + halfW), maxY: Math.round(camera.centerYCm + halfH) };
}

export function buildWorldStatePollUrl(layers: Partial<Pick<LayerToggles, OverlayLayerKey>>, viewportBounds?: ViewportWorldBoundsCm | null): string | null {
  const layerMask = computeStreamLayerMask(layers);
  const param = buildActiveLayersParam(layers);
  if (layerMask === 0 || param.length === 0) return null;
  const base = `/api/v1/world/state?layer_mask=${layerMask}&layers=${param}`;
  return viewportBounds ? `${base}&minX=${Math.round(viewportBounds.minX)}&minY=${Math.round(viewportBounds.minY)}&maxX=${Math.round(viewportBounds.maxX)}&maxY=${Math.round(viewportBounds.maxY)}` : base;
}

export function shouldFetchPoisCatalog(layers: Partial<Pick<LayerToggles, "pois">>): boolean { return layers.pois === true; }

export function computeLayerEntityCounts(state: Pick<WorldStreamState, "entities"> & { readonly pois?: readonly PoiOverlay[]; readonly cityNodes?: readonly VerraCityNodeMeta[] }): LayerEntityCounts {
  let players = 0, npcs = 0, monsters = 0, resource_nodes = 0, mounts = 0, loot_piles = 0, poiEntities = 0;
  for (const entity of state.entities.values()) {
    if (entity.kind === "player") players += 1;
    else if (entity.kind === "npc") npcs += 1;
    else if (entity.kind === "monster") monsters += 1;
    else if (entity.kind === "resource_node") resource_nodes += 1;
    else if (entity.kind === "mount") mounts += 1;
    else if (entity.kind === "loot_pile") loot_piles += 1;
    else if (entity.kind === "poi") poiEntities += 1;
  }
  const pois = (state.pois?.length ?? 0) + (state.cityNodes?.length ?? 0) + poiEntities;
  return { players, npcs, monsters, resource_nodes, resources: resource_nodes, mounts, loot_piles, pois };
}

function pruneHiddenLayerEntities(entities: ReadonlyMap<string, WorldEntity>, layers: LayerToggles): Map<string, WorldEntity> {
  const next = new Map<string, WorldEntity>();
  for (const [guid, entity] of entities.entries()) {
    if ((entity.kind === "player" && !layers.players) || (entity.kind === "npc" && !layers.npcs) || (entity.kind === "monster" && !layers.monsters)) continue;
    if ((entity.kind === "resource_node" && !layers.resource_nodes) || (entity.kind === "mount" && !layers.mounts) || (entity.kind === "loot_pile" && !layers.loot_piles) || (entity.kind === "poi" && !layers.pois)) continue;
    next.set(guid, entity);
  }
  return next;
}

export const DEFAULT_CAMERA_VIEWPORT: CameraViewport = Object.freeze({
  longitude: 0.0, latitude: 0.0, zoom: 3.5, pitch: 0.0, bearing: 0.0, mode: "2d", centerXCm: -777664.0, centerYCm: 522023.0,
});

export function parseTabFromUrlHash(hash: string): AdminTabId | null {
  const clean = hash.replace(/^#/, "").trim().toLowerCase();
  const match = /(?:^|&)tab=([^&]+)/.exec(clean);
  const candidate = match ? decodeURIComponent(match[1]) : clean.split("&")[0];
  if (candidate === "map" || candidate === "world-map") return "map";
  if (candidate === "analytics" || candidate === "analytics-state") return "analytics";
  if (candidate === "logs" || candidate === "players-spawner" || candidate === "logs-containers") return candidate;
  return null;
}

export function parseModalFromUrlHash(hash: string): GmModalId {
  const clean = hash.replace(/^#/, "").trim().toLowerCase();
  const match = /(?:^|&)modal=([^&]+)/.exec(clean);
  const candidate = match ? decodeURIComponent(match[1]) : "";
  if (candidate === "give-item" || candidate === "give-currency" || candidate === "teleport" || candidate === "spawn-npc" || candidate === "node-governance") return candidate;
  return null;
}

export function buildUrlHashForTab(tab: AdminTabId, modal: GmModalId = null): string {
  return modal ? `#tab=${tab}&modal=${modal}` : `#tab=${tab}`;
}

export function savePersistedWebAdminState(
  state: Pick<WorldStreamState, "activeTab" | "camera" | "layers" | "selectedPlayer" | "teleportMode" | "spawnTargetMode" | "selectedNpcTemplate" | "selectedContainer" | "logQuery"> & Partial<SessionUiExtraState> & {
    readonly activeModal?: GmModalId; readonly selectedEntityGuid?: string | null; readonly selectedCityNodeName?: string | null; readonly lastClickedPoint?: VerraUnrealPoint | null; readonly simulateAllNpcs?: boolean;
  },
  storage?: Pick<Storage, "setItem"> | null,
): PersistedWebAdminState {
  const entId = state.selectedEntityGuid ?? state.selectedEntityId ?? null;
  const snapshot: PersistedWebAdminState = {
    ...DEFAULT_SESSION_UI_EXTRA_STATE,
    ...state,
    activeTab: state.activeTab, activeModal: state.activeModal ?? null,
    camera: { centerXCm: state.camera.centerXCm, centerYCm: state.camera.centerYCm, zoom: state.camera.zoom, mode: state.camera.mode },
    layers: state.layers, layerMask: computeStreamLayerMask(state.layers),
    selectedPlayer: state.selectedPlayer, selectedEntityGuid: entId, selectedEntityId: entId,
    selectedCityNodeName: state.selectedCityNodeName ?? null, lastClickedPoint: state.lastClickedPoint ?? null,
    teleportMode: state.teleportMode, spawnTargetMode: state.spawnTargetMode,
    selectedNpcTemplate: state.selectedNpcTemplate, selectedDefId: state.selectedDefId ?? state.selectedNpcTemplate,
    selectedContainer: state.selectedContainer, logContainer: state.logContainer ?? state.selectedContainer,
    logQuery: state.logQuery, logSearch: state.logSearch ?? state.logQuery, simulateAllNpcs: state.simulateAllNpcs ?? true,
  };
  const targetStorage = resolveTabSessionStorage(storage);
  try { targetStorage?.setItem(SESSION_UI_STORAGE_KEY, JSON.stringify(snapshot)); } catch { /* ignore */ }
  return snapshot;
}

export const saveSessionUiState = savePersistedWebAdminState;

export function loadPersistedWebAdminState(storage?: Pick<Storage, "getItem"> | null, locationHash?: string, forceRestoreCamera = false): Partial<WorldStreamState> {
  const targetStorage = resolveTabSessionStorage(storage);
  const hashStr = locationHash !== undefined ? locationHash : typeof window !== "undefined" && window.location ? window.location.hash : "";
  let parsed: Partial<PersistedWebAdminState> = {};
  try { const raw = targetStorage?.getItem(SESSION_UI_STORAGE_KEY) ?? targetStorage?.getItem(WEB_ADMIN_STORAGE_KEY); if (raw) parsed = JSON.parse(raw) as Partial<PersistedWebAdminState>; } catch { parsed = {}; }
  const useDefaultCam = (!forceRestoreCamera && storage === undefined && !getActiveAdminJwtToken()) || !parsed.camera || (parsed.camera.zoom === 3.0 && parsed.camera.centerXCm === -777664.0 && parsed.camera.centerYCm === 522023.0);
  const entId = typeof (parsed.selectedEntityGuid ?? parsed.selectedEntityId) === "string" ? (parsed.selectedEntityGuid ?? parsed.selectedEntityId ?? null) : null;
  const selContainer = typeof parsed.selectedContainer === "string" && parsed.selectedContainer.length > 0 ? parsed.selectedContainer : "aoc-world-riverlands";
  const logQ = typeof (parsed.logQuery ?? parsed.logSearch) === "string" ? String(parsed.logQuery ?? parsed.logSearch) : "";
  const npcTmpl = typeof (parsed.selectedNpcTemplate ?? parsed.selectedDefId) === "string" && String(parsed.selectedNpcTemplate ?? parsed.selectedDefId).length > 0 ? String(parsed.selectedNpcTemplate ?? parsed.selectedDefId) : "Aela";
  return {
    ...DEFAULT_SESSION_UI_EXTRA_STATE,
    ...parsed,
    activeTab: parseTabFromUrlHash(hashStr) ?? (parsed.activeTab ?? "map"),
    activeModal: parseModalFromUrlHash(hashStr) ?? (parsed.activeModal ?? null),
    camera: useDefaultCam ? { ...DEFAULT_CAMERA_VIEWPORT } : { ...DEFAULT_CAMERA_VIEWPORT, ...parsed.camera },
    layers: parsed.layers ? { ...DEFAULT_LAYER_TOGGLES, ...parsed.layers } : DEFAULT_LAYER_TOGGLES,
    selectedPlayer: typeof parsed.selectedPlayer === "string" ? parsed.selectedPlayer : "",
    selectedEntityGuid: entId, selectedEntityId: entId,
    selectedCityNodeName: typeof parsed.selectedCityNodeName === "string" ? parsed.selectedCityNodeName : null,
    lastClickedPoint: parsed.lastClickedPoint ?? null, pickedCoordinate: parsed.lastClickedPoint ?? null,
    teleportMode: parsed.teleportMode ?? "place", spawnTargetMode: parsed.spawnTargetMode ?? "place",
    selectedNpcTemplate: npcTmpl, selectedDefId: npcTmpl,
    selectedContainer: selContainer, logContainer: typeof parsed.logContainer === "string" && parsed.logContainer.length > 0 ? parsed.logContainer : selContainer,
    logQuery: logQ, logSearch: logQ,
    collapsedSections: parsed.collapsedSections && typeof parsed.collapsedSections === "object" ? parsed.collapsedSections : {},
    simulateAllNpcs: typeof parsed.simulateAllNpcs === "boolean" ? parsed.simulateAllNpcs : true,
  };
}

export const restoreSessionUiState = loadPersistedWebAdminState;

function computeNextEntityMap(current: ReadonlyMap<string, WorldEntity>, delta: WorldDeltaBatch): Map<string, WorldEntity> {
  const next = delta.isSnapshot ? new Map<string, WorldEntity>() : new Map(current);
  for (const guid of delta.removed ?? []) next.delete(guid);
  for (const entity of delta.added ?? []) next.set(entity.guid, entity);
  for (const entity of delta.updated ?? []) { const prev = next.get(entity.guid); next.set(entity.guid, prev && !entity.name ? { ...entity, name: prev.name } : entity); }
  return next;
}

function computeDeltaPatch(state: WorldStreamState, delta: WorldDeltaBatch): Partial<WorldStreamState> | null {
  if (!Number.isFinite(delta.seq) || delta.seq < 0 || (!delta.isSnapshot && state.lastSeq > 0 && delta.seq <= state.lastSeq)) return null;
  const hasSeqGap = !delta.isSnapshot && state.lastSeq > 0 && delta.seq > state.lastSeq + 1;
  const nextEntities = computeNextEntityMap(state.entities, delta);
  const selGuid = state.selectedEntityGuid;
  const wasRemoved = selGuid ? (delta.removed ?? []).includes(selGuid) || (state.entities.has(selGuid) && !nextEntities.has(selGuid)) : false;
  const nextSelected = selGuid && !wasRemoved ? selGuid : null;
  const latency = typeof delta.tickLatencyMs === "number" && Number.isFinite(delta.tickLatencyMs) ? Math.max(0, delta.tickLatencyMs) : state.tickLatencyMs;
  const nextConn: StreamConnectionState = hasSeqGap || latency > UI_FRAME_BUDGET_MS * 2 ? "degraded" : "live";
  return { entities: nextEntities, selectedEntityGuid: nextSelected, selectedEntityId: nextSelected, lastSeq: delta.seq, tickLatencyMs: latency, needsSnapshotResync: delta.isSnapshot ? false : hasSeqGap, connectionState: state.connectionState === "replaying" ? "replaying" : nextConn, error: delta.isSnapshot ? null : state.error };
}

function updateNodeLevelInList(nodes: readonly VerraCityNodeMeta[], name: string, level: number): readonly VerraCityNodeMeta[] {
  const clampedLevel = Math.max(0, Math.min(6, Math.trunc(level)));
  return nodes.map((node) => (node.name === name || node.display_name === name ? { ...node, level: clampedLevel, plug_tier: nodeLevelToPlugTier(clampedLevel) } : node));
}

function mapElectionActionToStatus(action: NodeElectionAction): NodeElectionStatus {
  return action === "start" ? "registration" : action === "open_voting" ? "voting" : action === "conclude" ? "active_term" : "none";
}

function resolveCityNodeWorker(nodeName: string): ResolvedWorkerAuthority {
  const found = DEFAULT_VERRA_CITY_NODES.find((n) => n.name === nodeName || n.display_name === nodeName);
  const center = found ? getCityNodeCenterCm(found) : { cx_cm: -644519.0, cy_cm: 376773.0 };
  return resolveWorkerForCoordinateOrZone(center.cx_cm, center.cy_cm, undefined, undefined, nodeName);
}

let toastSeqCounter = 0;

function appendToastToList(list: readonly GmActionToast[], text: string, tone: "success" | "error" | "info", setState: WorldStreamStoreApi["setState"]): { readonly nextList: readonly GmActionToast[]; readonly toast: GmActionToast } {
  const now = Date.now(), clean = text.trim(), last = list[list.length - 1];
  if (last && last.text === clean && now - last.createdAtMs < 250) return { nextList: list, toast: last };
  toastSeqCounter += 1;
  const toast: GmActionToast = { id: `toast-${now}-${toastSeqCounter}`, text: clean, tone, createdAtMs: now };
  const nextList = [...list, toast].slice(-4);
  const timer = setTimeout(() => setState((prev) => ({ toasts: prev.toasts.filter((t) => t.id !== toast.id) })), 4500);
  (timer as unknown as { unref?: () => void })?.unref?.();
  return { nextList, toast };
}

async function executeSetNodeMayorAction(setState: WorldStreamStoreApi["setState"], nodeName: string, mayorCharacterGuidOrName: string, fetchImpl: typeof fetch = typeof fetch === "function" ? fetch : (undefined as unknown as typeof fetch)): Promise<CommandReceipt> {
  const cleanMayor = mayorCharacterGuidOrName.trim(), worker = resolveCityNodeWorker(nodeName);
  let ok = true, message = cleanMayor ? `Assigned ${cleanMayor} as mayor of ${nodeName}` : `Cleared mayor of ${nodeName}`;
  if (typeof fetchImpl === "function") {
    try {
      const res = await fetchImpl("/api/v1/gm/node/mayor", { method: "POST", headers: { "Content-Type": "application/json", Accept: "application/json" }, body: JSON.stringify({ node_name: nodeName, mayor_guid: cleanMayor, mayor_name: cleanMayor, zone: worker.scope_key, worker_id: worker.worker_id }) });
      const data = (await res.json().catch(() => ({}))) as Record<string, unknown>;
      ok = res.ok;
      if (typeof data.message === "string" && data.message.length > 0 && data.message !== "executed") message = data.message;
      else if (!res.ok) message = String(data.detail ?? data.title ?? `HTTP ${res.status}`);
    } catch { ok = true; }
  }
  const receipt: CommandReceipt = { scope: "nodeMayor", ok, message, timestampMs: Date.now() };
  setState((prev) => ({ cityNodes: prev.cityNodes.map((n) => (n.name === nodeName || n.display_name === nodeName ? { ...n, mayor_guid: cleanMayor, mayor_name: cleanMayor } : n)), lastReceiptsByScope: { ...prev.lastReceiptsByScope, nodeMayor: receipt }, toasts: appendToastToList(prev.toasts, message, ok ? "success" : "error", setState).nextList }));
  return receipt;
}

async function executeTriggerNodeElectionAction(setState: WorldStreamStoreApi["setState"], nodeName: string, action: NodeElectionAction, fetchImpl: typeof fetch = typeof fetch === "function" ? fetch : (undefined as unknown as typeof fetch)): Promise<CommandReceipt> {
  const worker = resolveCityNodeWorker(nodeName);
  const actionLabels: Record<NodeElectionAction, string> = { start: "Started election", open_voting: "Opened election voting", conclude: "Concluded election", cancel: "Cancelled election" };
  let nextStatus: string = mapElectionActionToStatus(action), ok = true, message = `${actionLabels[action]} in ${nodeName} (${nextStatus})`;
  if (typeof fetchImpl === "function") {
    try {
      const res = await fetchImpl("/api/v1/gm/node/election", { method: "POST", headers: { "Content-Type": "application/json", Accept: "application/json" }, body: JSON.stringify({ node_name: nodeName, action, zone: worker.scope_key, worker_id: worker.worker_id }) });
      const data = (await res.json().catch(() => ({}))) as Record<string, unknown>;
      ok = res.ok;
      if (typeof data.election_status === "string" && data.election_status.length > 0) nextStatus = data.election_status;
      if (typeof data.message === "string" && data.message.length > 0 && data.message !== "executed") message = data.message;
      else if (!res.ok) message = String(data.detail ?? data.title ?? `HTTP ${res.status}`);
    } catch { ok = true; }
  }
  const receipt: CommandReceipt = { scope: "nodeElection", ok, message, timestampMs: Date.now() };
  setState((prev) => ({ cityNodes: prev.cityNodes.map((n) => (n.name === nodeName || n.display_name === nodeName ? { ...n, election_status: nextStatus } : n)), lastReceiptsByScope: { ...prev.lastReceiptsByScope, nodeElection: receipt }, toasts: appendToastToList(prev.toasts, message, ok ? "success" : "error", setState).nextList }));
  return receipt;
}

async function runSetWorkerTickRateAction(setState: WorldStreamStoreApi["setState"], workerId: string, tickHz: number, fetchImpl: typeof fetch = typeof fetch === "function" ? fetch : (undefined as unknown as typeof fetch)): Promise<void> {
  const clampedHz = Math.max(1, Math.min(120, Math.round(Number.isFinite(tickHz) ? tickHz : 30)));
  setSubpartitionWorkerTickHz(workerId, clampedHz);
  setState((prev) => ({ workers: prev.workers.map((w) => (w.worker_id === workerId || w.canonical_name === workerId || w.container_name === workerId ? { ...w, tick_hz: clampedHz } : w)) }));
  if (typeof fetchImpl === "function") {
    await fetchImpl("/api/v1/gm/command", { method: "POST", headers: { "Content-Type": "application/json", Accept: "application/json" }, body: JSON.stringify({ command: "set_tick_rate", target: workerId, args: { worker_id: workerId, tick_hz: clampedHz }, params: { worker_id: workerId, tick_hz: clampedHz }, worker_id: workerId, tick_hz: clampedHz }) }).catch(() => {});
  }
  const message = `Set ${workerId} tick rate to ${clampedHz} Hz`;
  const receipt: CommandReceipt = { scope: "worker_tick", ok: true, message, timestampMs: Date.now() };
  setState((prev) => ({ lastReceiptsByScope: { ...prev.lastReceiptsByScope, worker_tick: receipt }, toasts: appendToastToList(prev.toasts, message, "success", setState).nextList }));
}

async function runSetTimeOfDayAction(setState: WorldStreamStoreApi["setState"], getState: () => WorldStreamState, hourInput: number | string, fetchImpl: typeof fetch = typeof fetch === "function" ? fetch : (undefined as unknown as typeof fetch)): Promise<void> {
  const cleanHour = parseTimeOfDayInput(hourInput);
  const label = formatTimeOfDayLabel(cleanHour);
  setState({ timeOfDayHours: cleanHour, timeOfDaySyncedAtMs: Date.now(), timeOfDayLabel: label });
  let message = `Set server time of day to ${label} (${cleanHour.toFixed(2)}h) across ${Math.max(1, getState().workers.length)} world workers`;
  if (typeof fetchImpl === "function") {
    const res = await fetchImpl("/api/v1/gm/commands", { method: "POST", headers: { "Content-Type": "application/json", Accept: "application/json" }, body: JSON.stringify({ command: "set_time_of_day", hour: cleanHour, params: { hour: cleanHour }, reason: "Operator updated server time of day" }) }).catch(() => null);
    const data = res ? ((await res.json().catch(() => ({}))) as Record<string, unknown>) : {};
    if (typeof data.message === "string" && data.message.length > 0 && data.message !== "executed") message = data.message;
  }
  const receipt: CommandReceipt = { scope: "timeOfDay", ok: true, message, timestampMs: Date.now() };
  setState((prev) => ({ lastReceiptsByScope: { ...prev.lastReceiptsByScope, timeOfDay: receipt }, toasts: appendToastToList(prev.toasts, message, "success", setState).nextList }));
}

function syncWindowUrlHash(tab: AdminTabId, modal: GmModalId): void {
  if (typeof window !== "undefined" && window.history?.replaceState) window.history.replaceState(null, "", buildUrlHashForTab(tab, modal));
}

let suppressSessionPersist = false;

function buildInitialWorldStreamState(setState: WorldStreamStoreApi["setState"], getState: () => WorldStreamState, overrides?: Partial<WorldStreamState>): WorldStreamState {
  const h = loadPersistedWebAdminState();
  return {
    ...DEFAULT_SESSION_UI_EXTRA_STATE, ...h,
    entities: new Map<string, WorldEntity>(), cityNodes: DEFAULT_VERRA_CITY_NODES, pois: [], workers: DEFAULT_REGIONAL_WORLD_WORKERS, toasts: [],
    selectedEntityGuid: h.selectedEntityGuid ?? null, selectedEntityId: h.selectedEntityGuid ?? null,
    selectedCityNodeName: h.selectedCityNodeName ?? null, selectedPoiGuid: null, selectedPlayer: h.selectedPlayer ?? "",
    activeTab: h.activeTab ?? "map", activeModal: h.activeModal ?? null, teleportMode: h.teleportMode ?? "place", spawnTargetMode: h.spawnTargetMode ?? "place",
    selectedNpcTemplate: h.selectedNpcTemplate ?? "Aela", selectedDefId: h.selectedNpcTemplate ?? "Aela",
    selectedContainer: h.selectedContainer ?? "aoc-world-riverlands", logContainer: h.logContainer ?? h.selectedContainer ?? "aoc-world-riverlands",
    logQuery: h.logQuery ?? "", logSearch: h.logQuery ?? "", simulateAllNpcs: h.simulateAllNpcs ?? true, layers: h.layers ?? DEFAULT_LAYER_TOGGLES, camera: h.camera ?? { ...DEFAULT_CAMERA_VIEWPORT },
    connectionState: "connecting", lastSeq: 0, tickLatencyMs: 0, streamHz: STREAM_TARGET_HZ, needsSnapshotResync: false, error: null,
    pickedCoordinate: h.lastClickedPoint ?? null, lastClickedPoint: h.lastClickedPoint ?? null, navmeshSegments: [], lastReceiptsByScope: DEFAULT_RECEIPTS_BY_SCOPE,
    pushToast: (text, tone = "info") => { const { nextList, toast } = appendToastToList(getState().toasts, text, tone, setState); setState({ toasts: nextList }); return toast; },
    dismissToast: (id) => setState((prev) => ({ toasts: prev.toasts.filter((t) => t.id !== id) })),
    applyWorldDelta: (delta) => { const patch = computeDeltaPatch(getState(), delta); if (patch) setState(patch); },
    selectEntity: (guid) => setState(guid !== null ? { selectedEntityGuid: guid, selectedEntityId: guid, selectedCityNodeName: null, selectedPoiGuid: null, pickedCoordinate: null, lastClickedPoint: null } : { selectedEntityGuid: null, selectedEntityId: null }),
    selectCityNode: (name) => setState(name !== null ? { selectedCityNodeName: name, selectedEntityGuid: null, selectedEntityId: null, selectedPoiGuid: null, pickedCoordinate: null, lastClickedPoint: null } : { selectedCityNodeName: null }),
    selectPoi: (guid) => setState(guid !== null ? { selectedPoiGuid: guid, selectedEntityGuid: null, selectedEntityId: null, selectedCityNodeName: null, pickedCoordinate: null, lastClickedPoint: null } : { selectedPoiGuid: null }),
    clearMapSelection: () => setState({ pickedCoordinate: null, lastClickedPoint: null, selectedEntityGuid: null, selectedEntityId: null, selectedCityNodeName: null, selectedPoiGuid: null }),
    setSelectedPlayer: (selectedPlayer) => setState({ selectedPlayer }),
    setActiveTab: (activeTab) => { syncWindowUrlHash(activeTab, getState().activeModal); setState({ activeTab }); },
    setActiveModal: (activeModal) => { syncWindowUrlHash(getState().activeTab, activeModal); setState({ activeModal }); },
    openCommandModal: (modal, point) => { const nextModal: GmModalId = modal === "spawn" ? "spawn-npc" : modal; syncWindowUrlHash(getState().activeTab, nextModal); setState({ activeModal: nextModal, ...(point ? { pickedCoordinate: point, lastClickedPoint: point } : {}), ...(nextModal === "teleport" ? { teleportMode: "coords" } : {}), ...(nextModal === "spawn-npc" ? { spawnTargetMode: "coords" } : {}) }); },
    setTeleportMode: (teleportMode) => setState({ teleportMode }), setSpawnTargetMode: (spawnTargetMode) => setState({ spawnTargetMode }),
    setSelectedNpcTemplate: (selectedNpcTemplate) => setState({ selectedNpcTemplate, selectedDefId: selectedNpcTemplate }),
    setSelectedContainer: (selectedContainer) => setState({ selectedContainer, logContainer: selectedContainer }),
    setLogQuery: (logQuery) => setState({ logQuery, logSearch: logQuery }),
    patchSessionUi: (patch) => setState({ ...patch, ...(patch.selectedEntityId !== undefined ? { selectedEntityGuid: patch.selectedEntityId } : {}), ...(patch.selectedDefId !== undefined ? { selectedNpcTemplate: patch.selectedDefId } : {}), ...(patch.logContainer !== undefined ? { selectedContainer: patch.logContainer } : {}), ...(patch.logSearch !== undefined ? { logQuery: patch.logSearch } : {}), ...(patch.logAutoTail !== undefined ? { autoRefreshLogs: patch.logAutoTail } : {}), ...(patch.autoRefreshLogs !== undefined && patch.logAutoTail === undefined ? { logAutoTail: patch.autoRefreshLogs } : {}) }),
    saveSessionUiState: (storage) => savePersistedWebAdminState(getState(), storage),
    restoreForAuthenticatedSession: (storage) => { const restored = loadPersistedWebAdminState(storage, "", true); const cur = getState(); setState({ ...restored, cityNodes: cur.cityNodes.length > 0 ? cur.cityNodes : DEFAULT_VERRA_CITY_NODES, workers: cur.workers.length > 0 ? cur.workers : DEFAULT_REGIONAL_WORLD_WORKERS, connectionState: "connecting" }); },
    setSimulateAllNpcs: async (enabled, fetchImpl = typeof fetch === "function" ? fetch : undefined) => { const nextVal = enabled !== undefined ? enabled : !getState().simulateAllNpcs; setState({ simulateAllNpcs: nextVal }); if (typeof fetchImpl === "function") await fetchImpl("/api/v1/gm/commands", { method: "POST", headers: { "Content-Type": "application/json", Accept: "application/json" }, body: JSON.stringify({ command: "set_simulate_all_npcs", params: { enabled: nextVal }, action: "set_simulate_all_npcs", enabled: nextVal }) }).catch(() => {}); return nextVal; },
    setCityNodes: (cityNodes) => setState({ cityNodes }), setPois: (pois) => setState({ pois }),
    setWorkers: (workers) => { const prev = getState().workers; if (workers.some((w) => (w.status === "UP" || w.status === "ONLINE") && prev.some((p) => p.worker_id === w.worker_id && p.status !== "UP" && p.status !== "ONLINE"))) resetAllRetryTimersAndCircuits("worker-ready"); setState({ workers }); },
    updateCityNodeLevel: (name, level) => setState((prev) => ({ cityNodes: updateNodeLevelInList(prev.cityNodes, name, level) })),
    setScopedReceipt: (scope, receiptOrMessage, tone) => setState((prev) => { const receipt = typeof receiptOrMessage === "string" ? { ok: tone !== "error", message: receiptOrMessage } : receiptOrMessage; const toastTone = tone ?? (receipt?.ok ? "success" : "error"); return { lastReceiptsByScope: { ...prev.lastReceiptsByScope, [scope]: receipt ? { scope, ok: receipt.ok, message: receipt.message, timestampMs: receipt.timestampMs ?? Date.now(), correlationId: receipt.correlationId } : null }, ...(receipt?.message ? { toasts: appendToastToList(prev.toasts, receipt.message, toastTone, setState).nextList } : {}) }; }),
    clearScopedReceipt: (scope) => setState((prev) => ({ lastReceiptsByScope: { ...prev.lastReceiptsByScope, [scope]: null } })),
    executeSetWorkerTickRateAction: (workerId, tickHz, fetchImpl) => runSetWorkerTickRateAction(setState, workerId, tickHz, fetchImpl),
    executeSetTimeOfDayAction: (hour, fetchImpl) => runSetTimeOfDayAction(setState, getState, hour, fetchImpl),
    setNodeMayor: (nodeName, mayor, fetchImpl) => executeSetNodeMayorAction(setState, nodeName, mayor, fetchImpl),
    triggerNodeElection: (nodeName, action, fetchImpl) => executeTriggerNodeElectionAction(setState, nodeName, action, fetchImpl),
    toggleLayer: (layer, enabled) => setState((prev) => { const nextVal = enabled !== undefined ? enabled : !prev.layers[layer]; const nextLayers: LayerToggles = { ...prev.layers, [layer]: nextVal, ...(layer === "pois" ? { cityNodes: nextVal } : {}), ...(layer === "cityNodes" ? { pois: nextVal } : {}) }; return { layers: nextLayers, entities: pruneHiddenLayerEntities(prev.entities, nextLayers) }; }),
    resetLayers: () => setState({ layers: DEFAULT_LAYER_TOGGLES }),
    setCameraViewport: (patch) => setState((prev) => ({ camera: { ...prev.camera, ...patch } })),
    setCameraMode: (mode) => setState((prev) => ({ camera: { ...prev.camera, mode, pitch: mode === "3d" ? Math.max(prev.camera.pitch, 50.0) : 0.0 } })),
    setConnectionState: (connectionState, error = null) => { if (connectionState === "live" && getState().connectionState !== "live") resetAllRetryTimersAndCircuits("service-live"); setState({ connectionState, error }); },
    setPickedCoordinate: (point) => setState(point !== null ? { pickedCoordinate: point, lastClickedPoint: point, selectedEntityGuid: null, selectedEntityId: null, selectedCityNodeName: null, selectedPoiGuid: null } : { pickedCoordinate: null, lastClickedPoint: null }),
    setSpatialOverlays: (navmeshSegments) => setState({ navmeshSegments }),
    resetForUnauthenticatedSession: () => { const cur = getState(); if (!suppressSessionPersist && cur.connectionState !== "offline") savePersistedWebAdminState(cur); suppressSessionPersist = true; setState({ ...DEFAULT_SESSION_UI_EXTRA_STATE, camera: { ...DEFAULT_CAMERA_VIEWPORT }, activeTab: "map", activeModal: null, entities: new Map<string, WorldEntity>(), pois: [], cityNodes: [], workers: [], navmeshSegments: [], pickedCoordinate: null, lastClickedPoint: null, selectedEntityGuid: null, selectedEntityId: null, selectedCityNodeName: null, selectedPoiGuid: null, selectedPlayer: "", connectionState: "offline" }); suppressSessionPersist = false; },
    ...overrides,
  };
}

export function createWorldStreamStore(initialOverrides?: Partial<WorldStreamState>): WorldStreamStoreApi {
  const listeners = new Set<(state: WorldStreamState, prevState: WorldStreamState) => void>();
  let state: WorldStreamState;
  const setState: WorldStreamStoreApi["setState"] = (partial) => {
    const patch = typeof partial === "function" ? partial(state) : partial;
    const syncedPatch: Partial<WorldStreamState> = {
      ...patch,
      ...(patch.pickedCoordinate !== undefined && patch.lastClickedPoint === undefined ? { lastClickedPoint: patch.pickedCoordinate } : {}),
      ...(patch.lastClickedPoint !== undefined && patch.pickedCoordinate === undefined ? { pickedCoordinate: patch.lastClickedPoint } : {}),
    };
    const nextState = Object.assign({}, state, syncedPatch);
    if (nextState === state) return;
    const prevState = state;
    state = nextState;
    if (!suppressSessionPersist && !isUnauthenticatedViewOnly() && state.connectionState !== "offline") {
      savePersistedWebAdminState(state);
    }
    for (const listener of listeners) listener(state, prevState);
  };
  state = buildInitialWorldStreamState(setState, () => state, initialOverrides);
  const initialState = state;
  return {
    getState: () => state,
    getInitialState: () => initialState,
    setState,
    subscribe: (listener) => { listeners.add(listener); return () => { listeners.delete(listener); }; },
  };
}

const defaultWorldStreamStore = createWorldStreamStore();
subscribeRetryReset((_ep, reason) => {
  if (reason === "logout" || reason === "unauthorized") defaultWorldStreamStore.getState().resetForUnauthenticatedSession();
  else if (reason === "login" || (reason === "refresh" && getActiveAdminJwtToken())) {
    defaultWorldStreamStore.getState().restoreForAuthenticatedSession();
  }
});

export function zoomToWorkerArea(worker: WorldWorkerInfo, storeSnap: WorldStreamState = defaultWorldStreamStore.getState()): WorkerSpatialArea {
  const area = resolveWorkerSpatialArea(worker);
  const canonical = worker.canonical_name ?? formatCanonicalWorkerName(worker.scope_kind, worker.scope_key);
  storeSnap.setCameraViewport({ centerXCm: area.centerXCm, centerYCm: area.centerYCm, zoom: area.targetZoom });
  storeSnap.pushToast(`Focused worker ${canonical} (${worker.region_label || worker.scope_key})`, "info");
  return area;
}

export const useWorldStreamStore: UseWorldStreamStoreHook = Object.assign(
  function useWorldStreamStoreSelector<U = WorldStreamState>(selector?: (state: WorldStreamState) => U): U {
    const useSync = (React as unknown as { useSyncExternalStore?: <T>(sub: (cb: () => void) => () => void, getSnap: () => T) => T }).useSyncExternalStore;
    const snap = typeof useSync === "function" ? useSync(defaultWorldStreamStore.subscribe, defaultWorldStreamStore.getState) : defaultWorldStreamStore.getState();
    return selector ? selector(snap) : (snap as unknown as U);
  },
  { getState: defaultWorldStreamStore.getState, getInitialState: defaultWorldStreamStore.getInitialState, setState: defaultWorldStreamStore.setState, subscribe: defaultWorldStreamStore.subscribe },
);
