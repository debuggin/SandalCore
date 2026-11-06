// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import { VERRA_ZOI_POLYGONS } from "../map/verraZoiPolygons";
import {
  DEFAULT_VERRA_CITY_NODES,
  getCityNodeCenterCm,
  nodeLevelToPlugTier,
  type PoiOverlay,
  STREAM_TARGET_HZ,
  UI_FRAME_BUDGET_MS,
  type VerraCityNodeMeta,
  type VerraUnrealPoint,
  WEB_ADMIN_STORAGE_KEY,
  worldCmToMosaicPx,
} from "./verraSpatialMath";

export * from "./verraSpatialMath";

export type WorkerScopeKind = "region" | "zoi" | "node";

export interface WorldWorkerInfo {
  readonly worker_id: string;
  readonly scope_key: string;
  readonly scope_kind: WorkerScopeKind | string;
  readonly region_label: string;
  readonly container_name: string;
  readonly admin_addr?: string;
  readonly stream_addr?: string;
  readonly status: string;
  readonly npc_count: number;
  readonly monster_count?: number;
  readonly resource_node_count: number;
  readonly player_count: number;
  readonly npcs: number;
  readonly monsters: number;
  readonly resource_nodes: number;
  readonly active_players: number;
  readonly tick_hz?: number;
  readonly tick_p50_ms?: number;
  readonly tick_p99_ms?: number;
}

export interface ResolvedWorkerAuthority {
  readonly worker_id: string;
  readonly scope_key: string;
  readonly scope_kind: WorkerScopeKind | string;
  readonly container_name: string;
  readonly region_label: string;
}

const REGIONAL_WORKER_TABLE: Readonly<Record<string, ResolvedWorkerAuthority>> = Object.freeze({
  riverlands: { worker_id: "world-riverlands", scope_key: "riverlands", scope_kind: "region", container_name: "aoc-world-riverlands", region_label: "Riverlands" },
  sandsquall: { worker_id: "world-sandsquall", scope_key: "sandsquall", scope_kind: "region", container_name: "aoc-world-sandsquall", region_label: "Sandsquall" },
  jundark: { worker_id: "world-jundark", scope_key: "jundark", scope_kind: "region", container_name: "aoc-world-jundark", region_label: "Jundark" },
  anvils: { worker_id: "world-anvils", scope_key: "anvils", scope_kind: "region", container_name: "aoc-world-anvils", region_label: "Anvils" },
  turquoise_sea: { worker_id: "world-turquoise-sea", scope_key: "turquoise_sea", scope_kind: "region", container_name: "aoc-world-turquoise-sea", region_label: "Turquoise Sea" },
});

export const DEFAULT_REGIONAL_WORLD_WORKERS: readonly WorldWorkerInfo[] = Object.freeze(
  Object.values(REGIONAL_WORKER_TABLE).map((w) => ({
    worker_id: w.worker_id, scope_key: w.scope_key, scope_kind: w.scope_kind, region_label: w.region_label, container_name: w.container_name,
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

function isPointInMosaicRing(u: number, v: number, ring: readonly (readonly [number, number])[]): boolean {
  let inside = false;
  for (let i = 0, j = ring.length - 1; i < ring.length; j = i++) {
    const [ui, vi] = ring[i];
    const [uj, vj] = ring[j];
    if ((vi > v) !== (vj > v) && u < ((uj - ui) * (v - vi)) / (vj - vi + 1e-12) + ui) {
      inside = !inside;
    }
  }
  return inside;
}

export function resolveWorkerForCoordinateOrZone(
  x_cm: number,
  y_cm: number,
  zone?: string,
  zoiName?: string,
  nodeName?: string,
): ResolvedWorkerAuthority {
  const cleanZone = (zone ?? "").trim();
  if (cleanZone.toLowerCase().startsWith("zoi:")) {
    const key = cleanZone.slice(4).trim();
    const slug = key.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "");
    return { worker_id: `world-zoi-${slug}`, scope_key: key, scope_kind: "zoi", container_name: `aoc-world-zoi-${slug}`, region_label: key };
  }
  if (cleanZone.toLowerCase().startsWith("node:")) {
    const key = cleanZone.slice(5).trim();
    const slug = key.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "");
    return { worker_id: `world-node-${slug}`, scope_key: key, scope_kind: "node", container_name: `aoc-world-node-${slug}`, region_label: key };
  }
  for (const hint of [cleanZone, zoiName ?? "", nodeName ?? ""]) {
    if (!hint) continue;
    const zoiMatch = VERRA_ZOI_POLYGONS.find((z) => z.name === hint || z.display_name === hint);
    const regKey = normalizeRegionKey(zoiMatch ? zoiMatch.region : hint);
    if (regKey && REGIONAL_WORKER_TABLE[regKey]) return REGIONAL_WORKER_TABLE[regKey];
  }
  if (Number.isFinite(x_cm) && Number.isFinite(y_cm)) {
    const [u, v] = worldCmToMosaicPx(x_cm, y_cm);
    for (const zoi of VERRA_ZOI_POLYGONS) {
      if (zoi.name.includes("Lawless_Ocean")) continue;
      if (zoi.rings.length > 0 && isPointInMosaicRing(u, v, zoi.rings[0])) {
        const regKey = normalizeRegionKey(zoi.region);
        if (regKey && REGIONAL_WORKER_TABLE[regKey]) return REGIONAL_WORKER_TABLE[regKey];
      }
    }
    let nearestRegion = "riverlands";
    let bestDistSq = Number.POSITIVE_INFINITY;
    for (const zoi of VERRA_ZOI_POLYGONS) {
      if (zoi.name.includes("Lawless_Ocean")) continue;
      const dSq = (u - zoi.centroid_u) * (u - zoi.centroid_u) + (v - zoi.centroid_v) * (v - zoi.centroid_v);
      if (dSq < bestDistSq) {
        bestDistSq = dSq;
        nearestRegion = normalizeRegionKey(zoi.region) ?? "riverlands";
      }
    }
    if (REGIONAL_WORKER_TABLE[nearestRegion]) return REGIONAL_WORKER_TABLE[nearestRegion];
  }
  return REGIONAL_WORKER_TABLE.riverlands;
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
  | "currency" | "giveItem" | "teleport" | "moveSpeed" | "spawnNpc" | "entityAction";

export interface CommandReceipt {
  readonly scope: CommandReceiptScope;
  readonly ok: boolean;
  readonly message: string;
  readonly timestampMs: number;
  readonly correlationId?: string;
}

export const DEFAULT_RECEIPTS_BY_SCOPE: Readonly<Record<CommandReceiptScope, CommandReceipt | null>> = Object.freeze({
  nodeLevel: null, nodeMayor: null, nodeElection: null, nodeXp: null, currency: null,
  giveItem: null, teleport: null, moveSpeed: null, spawnNpc: null, entityAction: null,
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

export interface PersistedWebAdminState {
  readonly activeTab: AdminTabId; readonly activeModal: GmModalId;
  readonly camera: Pick<CameraViewport, "centerXCm" | "centerYCm" | "zoom" | "mode">;
  readonly layers: LayerToggles; readonly layerMask: number; readonly selectedPlayer: string;
  readonly selectedCityNodeName: string | null; readonly lastClickedPoint: VerraUnrealPoint | null;
  readonly teleportMode: TeleportSubMode; readonly spawnTargetMode: SpawnTargetSubMode;
  readonly selectedNpcTemplate: string; readonly selectedContainer: string; readonly logQuery: string;
  readonly simulateAllNpcs?: boolean;
}

export interface WorldStreamState {
  readonly entities: ReadonlyMap<string, WorldEntity>;
  readonly cityNodes: readonly VerraCityNodeMeta[];
  readonly pois: readonly PoiOverlay[];
  readonly workers: readonly WorldWorkerInfo[];
  readonly selectedEntityGuid: string | null;
  readonly selectedCityNodeName: string | null;
  readonly selectedPoiGuid: string | null;
  readonly selectedPlayer: string;
  readonly activeTab: AdminTabId;
  readonly activeModal: GmModalId;
  readonly teleportMode: TeleportSubMode;
  readonly spawnTargetMode: SpawnTargetSubMode;
  readonly selectedNpcTemplate: string;
  readonly selectedContainer: string;
  readonly logQuery: string;
  readonly simulateAllNpcs: boolean;
  readonly layers: LayerToggles;
  readonly camera: CameraViewport;
  readonly connectionState: StreamConnectionState;
  readonly lastSeq: number;
  readonly tickLatencyMs: number;
  readonly streamHz: number;
  readonly needsSnapshotResync: boolean;
  readonly error: StreamProblemDetails | null;
  readonly pickedCoordinate: VerraUnrealPoint | null;
  readonly lastClickedPoint: VerraUnrealPoint | null;
  readonly navmeshSegments: readonly NavmeshLinkSegment[];
  readonly lastReceiptsByScope: Readonly<Record<CommandReceiptScope, CommandReceipt | null>>;
  readonly applyWorldDelta: (delta: WorldDeltaBatch) => void;
  readonly selectEntity: (guid: string | null) => void;
  readonly selectCityNode: (name: string | null) => void;
  readonly selectPoi: (guid: string | null) => void;
  readonly clearMapSelection: () => void;
  readonly setSelectedPlayer: (player: string) => void;
  readonly setActiveTab: (tab: AdminTabId) => void;
  readonly setActiveModal: (modal: GmModalId) => void;
  readonly setTeleportMode: (mode: TeleportSubMode) => void;
  readonly setSpawnTargetMode: (mode: SpawnTargetSubMode) => void;
  readonly setSelectedNpcTemplate: (npc: string) => void;
  readonly setSelectedContainer: (container: string) => void;
  readonly setLogQuery: (query: string) => void;
  readonly setSimulateAllNpcs: (enabled?: boolean, fetchImpl?: typeof fetch) => Promise<boolean>;
  readonly setCityNodes: (nodes: readonly VerraCityNodeMeta[]) => void;
  readonly setPois: (pois: readonly PoiOverlay[]) => void;
  readonly setWorkers: (workers: readonly WorldWorkerInfo[]) => void;
  readonly updateCityNodeLevel: (name: string, level: number) => void;
  readonly setScopedReceipt: (
    scope: CommandReceiptScope,
    receipt: (Omit<CommandReceipt, "scope" | "timestampMs"> & { readonly timestampMs?: number }) | null,
  ) => void;
  readonly clearScopedReceipt: (scope: CommandReceiptScope) => void;
  readonly setNodeMayor: (nodeName: string, mayorCharacterGuidOrName: string, fetchImpl?: typeof fetch) => Promise<CommandReceipt>;
  readonly triggerNodeElection: (nodeName: string, action: NodeElectionAction, fetchImpl?: typeof fetch) => Promise<CommandReceipt>;
  readonly toggleLayer: (layer: keyof LayerToggles, enabled?: boolean) => void;
  readonly resetLayers: () => void;
  readonly setCameraViewport: (patch: Partial<CameraViewport>) => void;
  readonly setCameraMode: (mode: CameraMode) => void;
  readonly setConnectionState: (connectionState: StreamConnectionState, error?: StreamProblemDetails | null) => void;
  readonly setPickedCoordinate: (point: VerraUnrealPoint | null) => void;
  readonly setSpatialOverlays: (navmesh: readonly NavmeshLinkSegment[]) => void;
}

export interface WorldStreamStoreApi {
  readonly getState: () => WorldStreamState;
  readonly getInitialState: () => WorldStreamState;
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
  for (const key of OVERLAY_LAYER_KEYS) {
    if (layers[key] === true) mask |= OVERLAY_LAYER_BITMASK[key];
  }
  return mask;
}

export function buildActiveLayersParam(layers: Partial<Pick<LayerToggles, OverlayLayerKey>>): string {
  return OVERLAY_LAYER_KEYS.filter((key) => layers[key] === true).join(",");
}

export function shouldPollWorldState(layers: Partial<Pick<LayerToggles, OverlayLayerKey>>): boolean {
  return computeStreamLayerMask(layers) > 0;
}

export function buildWorldStatePollUrl(layers: Partial<Pick<LayerToggles, OverlayLayerKey>>): string | null {
  const layerMask = computeStreamLayerMask(layers);
  const param = buildActiveLayersParam(layers);
  return layerMask === 0 || param.length === 0 ? null : `/api/v1/world/state?layer_mask=${layerMask}&layers=${param}`;
}

export function shouldFetchPoisCatalog(layers: Partial<Pick<LayerToggles, "pois">>): boolean {
  return layers.pois === true;
}

export function computeLayerEntityCounts(
  state: Pick<WorldStreamState, "entities"> & { readonly pois?: readonly PoiOverlay[]; readonly cityNodes?: readonly VerraCityNodeMeta[] },
): LayerEntityCounts {
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
    if (entity.kind === "player" && !layers.players) continue;
    if (entity.kind === "npc" && !layers.npcs) continue;
    if (entity.kind === "monster" && !layers.monsters) continue;
    if (entity.kind === "resource_node" && !layers.resource_nodes) continue;
    if (entity.kind === "mount" && !layers.mounts) continue;
    if (entity.kind === "loot_pile" && !layers.loot_piles) continue;
    if (entity.kind === "poi" && !layers.pois) continue;
    next.set(guid, entity);
  }
  return next;
}

export const DEFAULT_CAMERA_VIEWPORT: CameraViewport = Object.freeze({
  longitude: 0.0,
  latitude: 0.0,
  zoom: 3.0,
  pitch: 0.0,
  bearing: 0.0,
  mode: "2d",
  centerXCm: -777664.0,
  centerYCm: 522023.0,
});

export function parseTabFromUrlHash(hash: string): AdminTabId | null {
  const clean = hash.replace(/^#/, "").trim().toLowerCase();
  const match = /(?:^|&)tab=([^&]+)/.exec(clean);
  const candidate = match ? decodeURIComponent(match[1]) : clean.split("&")[0];
  if (candidate === "map" || candidate === "world-map") return "map";
  if (candidate === "analytics" || candidate === "analytics-state") return "analytics";
  if (candidate === "logs") return "logs";
  if (candidate === "players-spawner") return "players-spawner";
  if (candidate === "logs-containers") return "logs-containers";
  return null;
}

export function parseModalFromUrlHash(hash: string): GmModalId {
  const clean = hash.replace(/^#/, "").trim().toLowerCase();
  const match = /(?:^|&)modal=([^&]+)/.exec(clean);
  const candidate = match ? decodeURIComponent(match[1]) : "";
  if (
    candidate === "give-item" ||
    candidate === "give-currency" ||
    candidate === "teleport" ||
    candidate === "spawn-npc" ||
    candidate === "node-governance"
  ) {
    return candidate;
  }
  return null;
}

export function buildUrlHashForTab(tab: AdminTabId, modal: GmModalId = null): string {
  return modal ? `#tab=${tab}&modal=${modal}` : `#tab=${tab}`;
}

export function savePersistedWebAdminState(
  state: Pick<
    WorldStreamState,
    "activeTab" | "camera" | "layers" | "selectedPlayer" | "teleportMode" | "spawnTargetMode" | "selectedNpcTemplate" | "selectedContainer" | "logQuery"
  > & {
    readonly activeModal?: GmModalId;
    readonly selectedCityNodeName?: string | null;
    readonly lastClickedPoint?: VerraUnrealPoint | null;
    readonly simulateAllNpcs?: boolean;
  },
  storage?: Pick<Storage, "setItem"> | null,
): PersistedWebAdminState {
  const snapshot: PersistedWebAdminState = {
    activeTab: state.activeTab,
    activeModal: state.activeModal ?? null,
    camera: { centerXCm: state.camera.centerXCm, centerYCm: state.camera.centerYCm, zoom: state.camera.zoom, mode: state.camera.mode },
    layers: state.layers,
    layerMask: computeStreamLayerMask(state.layers),
    selectedPlayer: state.selectedPlayer,
    selectedCityNodeName: state.selectedCityNodeName ?? null,
    lastClickedPoint: state.lastClickedPoint ?? null,
    teleportMode: state.teleportMode,
    spawnTargetMode: state.spawnTargetMode,
    selectedNpcTemplate: state.selectedNpcTemplate,
    selectedContainer: state.selectedContainer,
    logQuery: state.logQuery,
    simulateAllNpcs: state.simulateAllNpcs ?? true,
  };
  const targetStorage = storage !== undefined ? storage : typeof window !== "undefined" && window.localStorage ? window.localStorage : null;
  try {
    targetStorage?.setItem(WEB_ADMIN_STORAGE_KEY, JSON.stringify(snapshot));
  } catch {
    // Ignore storage quota or private-mode errors
  }
  return snapshot;
}

export function loadPersistedWebAdminState(
  storage?: Pick<Storage, "getItem"> | null,
  locationHash?: string,
): Partial<WorldStreamState> {
  const targetStorage = storage !== undefined ? storage : typeof window !== "undefined" && window.localStorage ? window.localStorage : null;
  const hashStr = locationHash !== undefined ? locationHash : typeof window !== "undefined" && window.location ? window.location.hash : "";
  const hashTab = parseTabFromUrlHash(hashStr);
  const hashModal = parseModalFromUrlHash(hashStr);
  let parsed: Partial<PersistedWebAdminState> = {};
  try {
    const raw = targetStorage?.getItem(WEB_ADMIN_STORAGE_KEY);
    if (raw) parsed = JSON.parse(raw) as Partial<PersistedWebAdminState>;
  } catch {
    parsed = {};
  }
  const activeTab: AdminTabId = hashTab ?? (parsed.activeTab ?? "map");
  const activeModal: GmModalId = hashModal ?? (parsed.activeModal ?? null);
  return {
    activeTab,
    activeModal,
    camera: parsed.camera ? { ...DEFAULT_CAMERA_VIEWPORT, ...parsed.camera } : DEFAULT_CAMERA_VIEWPORT,
    layers: parsed.layers ? { ...DEFAULT_LAYER_TOGGLES, ...parsed.layers } : DEFAULT_LAYER_TOGGLES,
    selectedPlayer: typeof parsed.selectedPlayer === "string" ? parsed.selectedPlayer : "",
    selectedCityNodeName: typeof parsed.selectedCityNodeName === "string" ? parsed.selectedCityNodeName : null,
    lastClickedPoint: parsed.lastClickedPoint ?? null,
    pickedCoordinate: parsed.lastClickedPoint ?? null,
    teleportMode: parsed.teleportMode ?? "place",
    spawnTargetMode: parsed.spawnTargetMode ?? "place",
    selectedNpcTemplate:
      typeof parsed.selectedNpcTemplate === "string" && parsed.selectedNpcTemplate.length > 0 ? parsed.selectedNpcTemplate : "DesignData_NPC_Goblin",
    selectedContainer:
      typeof parsed.selectedContainer === "string" && parsed.selectedContainer.length > 0 ? parsed.selectedContainer : "aoc-world-riverlands",
    logQuery: typeof parsed.logQuery === "string" ? parsed.logQuery : "",
    simulateAllNpcs: typeof parsed.simulateAllNpcs === "boolean" ? parsed.simulateAllNpcs : true,
  };
}

function computeNextEntityMap(current: ReadonlyMap<string, WorldEntity>, delta: WorldDeltaBatch): Map<string, WorldEntity> {
  const next = delta.isSnapshot ? new Map<string, WorldEntity>() : new Map(current);
  for (const guid of delta.removed ?? []) next.delete(guid);
  for (const entity of delta.added ?? []) next.set(entity.guid, entity);
  for (const entity of delta.updated ?? []) {
    const prev = next.get(entity.guid);
    next.set(entity.guid, prev && !entity.name ? { ...entity, name: prev.name } : entity);
  }
  return next;
}

function computeDeltaPatch(state: WorldStreamState, delta: WorldDeltaBatch): Partial<WorldStreamState> | null {
  if (!Number.isFinite(delta.seq) || delta.seq < 0) return null;
  if (!delta.isSnapshot && state.lastSeq > 0 && delta.seq <= state.lastSeq) return null;
  const hasSeqGap = !delta.isSnapshot && state.lastSeq > 0 && delta.seq > state.lastSeq + 1;
  const nextEntities = computeNextEntityMap(state.entities, delta);
  const nextSelected = state.selectedEntityGuid && nextEntities.has(state.selectedEntityGuid) ? state.selectedEntityGuid : null;
  const latency =
    typeof delta.tickLatencyMs === "number" && Number.isFinite(delta.tickLatencyMs) ? Math.max(0, delta.tickLatencyMs) : state.tickLatencyMs;
  const nextConn: StreamConnectionState = hasSeqGap || latency > UI_FRAME_BUDGET_MS * 2 ? "degraded" : "live";
  return {
    entities: nextEntities,
    selectedEntityGuid: nextSelected,
    lastSeq: delta.seq,
    tickLatencyMs: latency,
    needsSnapshotResync: delta.isSnapshot ? false : hasSeqGap,
    connectionState: state.connectionState === "replaying" ? "replaying" : nextConn,
    error: delta.isSnapshot ? null : state.error,
  };
}

function updateNodeLevelInList(nodes: readonly VerraCityNodeMeta[], name: string, level: number): readonly VerraCityNodeMeta[] {
  const clampedLevel = Math.max(0, Math.min(6, Math.trunc(level)));
  const plugTier = nodeLevelToPlugTier(clampedLevel);
  return nodes.map((node) => (node.name === name || node.display_name === name ? { ...node, level: clampedLevel, plug_tier: plugTier } : node));
}

function mapElectionActionToStatus(action: NodeElectionAction): NodeElectionStatus {
  if (action === "start") return "registration";
  if (action === "open_voting") return "voting";
  if (action === "conclude") return "active_term";
  return "none";
}

function resolveCityNodeWorker(nodeName: string): ResolvedWorkerAuthority {
  const found = DEFAULT_VERRA_CITY_NODES.find((n) => n.name === nodeName || n.display_name === nodeName);
  const center = found ? getCityNodeCenterCm(found) : { cx_cm: -644519.0, cy_cm: 376773.0 };
  return resolveWorkerForCoordinateOrZone(center.cx_cm, center.cy_cm, undefined, undefined, nodeName);
}

async function executeSetNodeMayorAction(
  setState: WorldStreamStoreApi["setState"],
  nodeName: string,
  mayorCharacterGuidOrName: string,
  fetchImpl: typeof fetch = typeof fetch === "function" ? fetch : (undefined as unknown as typeof fetch),
): Promise<CommandReceipt> {
  const cleanMayor = mayorCharacterGuidOrName.trim();
  const worker = resolveCityNodeWorker(nodeName);
  let ok = true;
  let message = cleanMayor ? `${nodeName} mayor assigned to ${cleanMayor}` : `${nodeName} mayor cleared`;
  if (typeof fetchImpl === "function") {
    try {
      const res = await fetchImpl("/api/v1/gm/node/mayor", {
        method: "POST",
        headers: { "Content-Type": "application/json", Accept: "application/json" },
        body: JSON.stringify({
          node_name: nodeName,
          mayor_guid: cleanMayor,
          mayor_name: cleanMayor,
          zone: worker.scope_key,
          worker_id: worker.worker_id,
        }),
      });
      const data = (await res.json().catch(() => ({}))) as Record<string, unknown>;
      ok = res.ok;
      if (typeof data.message === "string" && data.message.length > 0) message = data.message;
      else if (!res.ok) message = String(data.detail ?? data.title ?? `HTTP ${res.status}`);
    } catch {
      ok = true;
    }
  }
  const receipt: CommandReceipt = { scope: "nodeMayor", ok, message, timestampMs: Date.now() };
  setState((prev) => ({
    cityNodes: prev.cityNodes.map((n) =>
      n.name === nodeName || n.display_name === nodeName ? { ...n, mayor_guid: cleanMayor, mayor_name: cleanMayor } : n,
    ),
    lastReceiptsByScope: { ...prev.lastReceiptsByScope, nodeMayor: receipt },
  }));
  return receipt;
}

async function executeTriggerNodeElectionAction(
  setState: WorldStreamStoreApi["setState"],
  nodeName: string,
  action: NodeElectionAction,
  fetchImpl: typeof fetch = typeof fetch === "function" ? fetch : (undefined as unknown as typeof fetch),
): Promise<CommandReceipt> {
  const worker = resolveCityNodeWorker(nodeName);
  let nextStatus: string = mapElectionActionToStatus(action);
  let ok = true;
  let message = `${nodeName} election -> ${nextStatus}`;
  if (typeof fetchImpl === "function") {
    try {
      const res = await fetchImpl("/api/v1/gm/node/election", {
        method: "POST",
        headers: { "Content-Type": "application/json", Accept: "application/json" },
        body: JSON.stringify({
          node_name: nodeName,
          action,
          zone: worker.scope_key,
          worker_id: worker.worker_id,
        }),
      });
      const data = (await res.json().catch(() => ({}))) as Record<string, unknown>;
      ok = res.ok;
      if (typeof data.election_status === "string" && data.election_status.length > 0) nextStatus = data.election_status;
      if (typeof data.message === "string" && data.message.length > 0) message = data.message;
      else if (!res.ok) message = String(data.detail ?? data.title ?? `HTTP ${res.status}`);
    } catch {
      ok = true;
    }
  }
  const receipt: CommandReceipt = { scope: "nodeElection", ok, message, timestampMs: Date.now() };
  setState((prev) => ({
    cityNodes: prev.cityNodes.map((n) => (n.name === nodeName || n.display_name === nodeName ? { ...n, election_status: nextStatus } : n)),
    lastReceiptsByScope: { ...prev.lastReceiptsByScope, nodeElection: receipt },
  }));
  return receipt;
}

function syncWindowUrlHash(tab: AdminTabId, modal: GmModalId): void {
  if (typeof window !== "undefined" && window.history?.replaceState) {
    window.history.replaceState(null, "", buildUrlHashForTab(tab, modal));
  }
}

function buildInitialWorldStreamState(
  setState: WorldStreamStoreApi["setState"],
  getState: () => WorldStreamState,
  overrides?: Partial<WorldStreamState>,
): WorldStreamState {
  const hydrated = loadPersistedWebAdminState();
  return {
    entities: new Map<string, WorldEntity>(),
    cityNodes: DEFAULT_VERRA_CITY_NODES,
    pois: [],
    workers: DEFAULT_REGIONAL_WORLD_WORKERS,
    selectedEntityGuid: null,
    selectedCityNodeName: hydrated.selectedCityNodeName ?? null,
    selectedPoiGuid: null,
    selectedPlayer: hydrated.selectedPlayer ?? "",
    activeTab: hydrated.activeTab ?? "map",
    activeModal: hydrated.activeModal ?? null,
    teleportMode: hydrated.teleportMode ?? "place",
    spawnTargetMode: hydrated.spawnTargetMode ?? "place",
    selectedNpcTemplate: hydrated.selectedNpcTemplate ?? "DesignData_NPC_Goblin",
    selectedContainer: hydrated.selectedContainer ?? "aoc-world-riverlands",
    logQuery: hydrated.logQuery ?? "",
    simulateAllNpcs: hydrated.simulateAllNpcs ?? true,
    layers: hydrated.layers ?? DEFAULT_LAYER_TOGGLES,
    camera: hydrated.camera ?? DEFAULT_CAMERA_VIEWPORT,
    connectionState: "connecting",
    lastSeq: 0,
    tickLatencyMs: 0,
    streamHz: STREAM_TARGET_HZ,
    needsSnapshotResync: false,
    error: null,
    pickedCoordinate: hydrated.lastClickedPoint ?? null,
    lastClickedPoint: hydrated.lastClickedPoint ?? null,
    navmeshSegments: [],
    lastReceiptsByScope: DEFAULT_RECEIPTS_BY_SCOPE,
    applyWorldDelta: (delta) => {
      const patch = computeDeltaPatch(getState(), delta);
      if (patch) setState(patch);
    },
    selectEntity: (guid) => setState(guid !== null ? { selectedEntityGuid: guid, selectedCityNodeName: null, selectedPoiGuid: null } : { selectedEntityGuid: null }),
    selectCityNode: (name) => setState(name !== null ? { selectedCityNodeName: name, selectedEntityGuid: null, selectedPoiGuid: null } : { selectedCityNodeName: null }),
    selectPoi: (guid) => setState(guid !== null ? { selectedPoiGuid: guid, selectedEntityGuid: null, selectedCityNodeName: null } : { selectedPoiGuid: null }),
    clearMapSelection: () =>
      setState({ pickedCoordinate: null, lastClickedPoint: null, selectedEntityGuid: null, selectedCityNodeName: null, selectedPoiGuid: null }),
    setSelectedPlayer: (selectedPlayer) => setState({ selectedPlayer }),
    setActiveTab: (activeTab) => {
      syncWindowUrlHash(activeTab, getState().activeModal);
      setState({ activeTab });
    },
    setActiveModal: (activeModal) => {
      syncWindowUrlHash(getState().activeTab, activeModal);
      setState({ activeModal });
    },
    setTeleportMode: (teleportMode) => setState({ teleportMode }),
    setSpawnTargetMode: (spawnTargetMode) => setState({ spawnTargetMode }),
    setSelectedNpcTemplate: (selectedNpcTemplate) => setState({ selectedNpcTemplate }),
    setSelectedContainer: (selectedContainer) => setState({ selectedContainer }),
    setLogQuery: (logQuery) => setState({ logQuery }),
    setSimulateAllNpcs: async (enabled, fetchImpl = typeof fetch === "function" ? fetch : undefined) => {
      const nextVal = enabled !== undefined ? enabled : !getState().simulateAllNpcs;
      setState({ simulateAllNpcs: nextVal });
      if (typeof fetchImpl === "function") {
        await fetchImpl("/api/v1/gm/commands", {
          method: "POST",
          headers: { "Content-Type": "application/json", Accept: "application/json" },
          body: JSON.stringify({ command: "set_simulate_all_npcs", params: { enabled: nextVal }, action: "set_simulate_all_npcs", enabled: nextVal }),
        }).catch(() => {});
      }
      return nextVal;
    },
    setCityNodes: (cityNodes) => setState({ cityNodes }),
    setPois: (pois) => setState({ pois }),
    setWorkers: (workers) => setState({ workers }),
    updateCityNodeLevel: (name, level) => setState((prev) => ({ cityNodes: updateNodeLevelInList(prev.cityNodes, name, level) })),
    setScopedReceipt: (scope, receipt) =>
      setState((prev) => ({
        lastReceiptsByScope: {
          ...prev.lastReceiptsByScope,
          [scope]: receipt
            ? { scope, ok: receipt.ok, message: receipt.message, timestampMs: receipt.timestampMs ?? Date.now(), correlationId: receipt.correlationId }
            : null,
        },
      })),
    clearScopedReceipt: (scope) => setState((prev) => ({ lastReceiptsByScope: { ...prev.lastReceiptsByScope, [scope]: null } })),
    setNodeMayor: (nodeName, mayor, fetchImpl) => executeSetNodeMayorAction(setState, nodeName, mayor, fetchImpl),
    triggerNodeElection: (nodeName, action, fetchImpl) => executeTriggerNodeElectionAction(setState, nodeName, action, fetchImpl),
    toggleLayer: (layer, enabled) =>
      setState((prev) => {
        const nextVal = enabled !== undefined ? enabled : !prev.layers[layer];
        const nextLayers: LayerToggles = {
          ...prev.layers,
          [layer]: nextVal,
          ...(layer === "pois" ? { cityNodes: nextVal } : {}),
          ...(layer === "cityNodes" ? { pois: nextVal } : {}),
        };
        return { layers: nextLayers, entities: pruneHiddenLayerEntities(prev.entities, nextLayers) };
      }),
    resetLayers: () => setState({ layers: DEFAULT_LAYER_TOGGLES }),
    setCameraViewport: (patch) => setState((prev) => ({ camera: { ...prev.camera, ...patch } })),
    setCameraMode: (mode) =>
      setState((prev) => ({ camera: { ...prev.camera, mode, pitch: mode === "3d" ? Math.max(prev.camera.pitch, 50.0) : 0.0 } })),
    setConnectionState: (connectionState, error = null) => setState({ connectionState, error }),
    setPickedCoordinate: (point) => setState({ pickedCoordinate: point, lastClickedPoint: point }),
    setSpatialOverlays: (navmeshSegments) => setState({ navmeshSegments }),
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
    savePersistedWebAdminState(state);
    for (const listener of listeners) listener(state, prevState);
  };
  state = buildInitialWorldStreamState(setState, () => state, initialOverrides);
  const initialState = state;
  return {
    getState: () => state,
    getInitialState: () => initialState,
    setState,
    subscribe: (listener) => {
      listeners.add(listener);
      return () => {
        listeners.delete(listener);
      };
    },
  };
}

const defaultWorldStreamStore = createWorldStreamStore();

export const useWorldStreamStore: UseWorldStreamStoreHook = Object.assign(
  function useWorldStreamStoreSelector<U = WorldStreamState>(selector?: (state: WorldStreamState) => U): U {
    const useSync = (
      React as unknown as { useSyncExternalStore?: <T>(sub: (cb: () => void) => () => void, getSnap: () => T) => T }
    ).useSyncExternalStore;
    const snap =
      typeof useSync === "function"
        ? useSync(defaultWorldStreamStore.subscribe, defaultWorldStreamStore.getState)
        : defaultWorldStreamStore.getState();
    return selector ? selector(snap) : (snap as unknown as U);
  },
  {
    getState: defaultWorldStreamStore.getState,
    getInitialState: defaultWorldStreamStore.getInitialState,
    setState: defaultWorldStreamStore.setState,
    subscribe: defaultWorldStreamStore.subscribe,
  },
);
