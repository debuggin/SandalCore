// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";

export const VERRA_EXTENT_CM = 1_200_000.0;
export const VERRA_MAX_LNG_DEG = 180.0;
export const VERRA_MAX_LAT_DEG = 85.05112877980659;
export const STREAM_TARGET_HZ = 10;
export const UI_FRAME_BUDGET_MS = 16.6;

export const VERRA_MAP_X0 = -47;
export const VERRA_MAP_Y0 = -17;
export const VERRA_MAP_COLS = 51;
export const VERRA_MAP_ROWS = 46;
export const VERRA_MAP_SOURCE_CM = 50000;
export const VERRA_MAP_SOURCE_PX = 512;
export const VERRA_MAP_TILE_PX = 256;
export const VERRA_MAP_WIDTH_PX = 26112;
export const VERRA_MAP_HEIGHT_PX = 23552;
export const VERRA_MAP_MAX_ZOOM = 7;
export const VERRA_MAP_PLUG_ZOOM = 8;
export const VERRA_NODE_PLUG_SIZE_CM = 100000;
export const VERRA_NODE_PLUG_SIZE_PX = 1024;

export const NODE_STAGE_NAMES: readonly string[] = Object.freeze([
  "Wilderness",
  "Expedition",
  "Encampment",
  "Village",
  "Town",
  "City",
  "Metropolis",
]);

const NODE_PLUG_TIER_BY_LEVEL: readonly number[] = Object.freeze([
  -1, 0, 1, 2, 3, 4, 4,
]);

export type LngLatTuple = readonly [number, number];
export type MosaicPxTuple = readonly [number, number];
export type WorldCmTuple = readonly [number, number];
export type AdminTabId = "map" | "players-spawner" | "logs-containers";

export interface VerraUnrealPoint {
  readonly xCm: number;
  readonly yCm: number;
  readonly zCm: number;
}

export type WorldEntityKind =
  | "player"
  | "npc"
  | "monster"
  | "poi"
  | "spawn"
  | "resource_node"
  | "mount"
  | "loot_pile";

export const STREAM_LAYER_PLAYERS = 1;
export const STREAM_LAYER_NPCS = 2;
export const STREAM_LAYER_RESOURCE_NODES = 4;
export const STREAM_LAYER_MOUNTS = 8;
export const STREAM_LAYER_LOOT_PILES = 16;
export const STREAM_LAYER_ALL = 31;
export const LAYER_POIS = 32;
export const LAYER_MONSTERS = 64;

export const OVERLAY_LAYER_KEYS = Object.freeze([
  "players",
  "npcs",
  "monsters",
  "resource_nodes",
  "mounts",
  "loot_piles",
  "pois",
] as const);

export type OverlayLayerKey = (typeof OVERLAY_LAYER_KEYS)[number];

export const OVERLAY_LAYER_BITMASK: Readonly<Record<OverlayLayerKey, number>> =
  Object.freeze({
    players: STREAM_LAYER_PLAYERS,
    npcs: STREAM_LAYER_NPCS,
    monsters: LAYER_MONSTERS,
    resource_nodes: STREAM_LAYER_RESOURCE_NODES,
    mounts: STREAM_LAYER_MOUNTS,
    loot_piles: STREAM_LAYER_LOOT_PILES,
    pois: LAYER_POIS,
  });

export interface WorldEntity {
  readonly guid: string;
  readonly name: string;
  readonly kind: WorldEntityKind;
  readonly xCm: number;
  readonly yCm: number;
  readonly zCm: number;
  readonly yawDeg: number;
  readonly level: number;
  readonly healthPct: number;
  readonly inCombat?: boolean;
  readonly dead?: boolean;
  readonly mounted?: boolean;
  readonly zoneId?: string;
}

export interface VerraCityNodeMeta {
  readonly node_index: number;
  readonly guid: string;
  readonly name: string;
  readonly display_name: string;
  readonly ox: number;
  readonly oy: number;
  readonly oz?: number;
  readonly yaw_deg: number;
  readonly size_cm: number;
  readonly level: number;
  readonly plug_tier: number;
}

export interface VerraMapMeta {
  readonly x0: number;
  readonly y0: number;
  readonly cols: number;
  readonly rows: number;
  readonly source_cm: number;
  readonly source_px: number;
  readonly tile_px: number;
  readonly width_px: number;
  readonly height_px: number;
  readonly max_zoom: number;
  readonly plug_zoom: number;
  readonly nodes: readonly VerraCityNodeMeta[];
}

export interface LayerToggles {
  readonly players: boolean;
  readonly npcs: boolean;
  readonly monsters: boolean;
  readonly resource_nodes: boolean;
  readonly mounts: boolean;
  readonly loot_piles: boolean;
  readonly pois: boolean;
  readonly cityNodes: boolean;
  readonly nodePlugs: boolean;
  readonly spawns: boolean;
  readonly navmesh: boolean;
}

export type CameraMode = "2d" | "3d";

export interface CameraViewport {
  readonly longitude: number;
  readonly latitude: number;
  readonly zoom: number;
  readonly pitch: number;
  readonly bearing: number;
  readonly mode: CameraMode;
  readonly centerXCm: number;
  readonly centerYCm: number;
}

export type StreamConnectionState =
  | "connecting"
  | "live"
  | "degraded"
  | "offline"
  | "replaying";

export interface StreamProblemDetails {
  readonly status: number;
  readonly title: string;
  readonly detail: string;
  readonly correlationId?: string;
  readonly retryAfterSeconds?: number;
}

export interface WorldDeltaBatch {
  readonly seq: number;
  readonly tick?: number;
  readonly tickLatencyMs?: number;
  readonly isSnapshot?: boolean;
  readonly added?: readonly WorldEntity[];
  readonly updated?: readonly WorldEntity[];
  readonly removed?: readonly string[];
}

export interface NavmeshLinkSegment {
  readonly segmentId: string;
  readonly fromCm: readonly [number, number, number];
  readonly toCm: readonly [number, number, number];
}

export interface WorldStreamState {
  readonly entities: ReadonlyMap<string, WorldEntity>;
  readonly cityNodes: readonly VerraCityNodeMeta[];
  readonly selectedEntityGuid: string | null;
  readonly selectedCityNodeName: string | null;
  readonly selectedPlayer: string;
  readonly activeTab: AdminTabId;
  readonly layers: LayerToggles;
  readonly camera: CameraViewport;
  readonly connectionState: StreamConnectionState;
  readonly lastSeq: number;
  readonly tickLatencyMs: number;
  readonly streamHz: number;
  readonly needsSnapshotResync: boolean;
  readonly error: StreamProblemDetails | null;
  readonly pickedCoordinate: VerraUnrealPoint | null;
  readonly navmeshSegments: readonly NavmeshLinkSegment[];
  readonly applyWorldDelta: (delta: WorldDeltaBatch) => void;
  readonly selectEntity: (guid: string | null) => void;
  readonly selectCityNode: (name: string | null) => void;
  readonly setSelectedPlayer: (player: string) => void;
  readonly setActiveTab: (tab: AdminTabId) => void;
  readonly setCityNodes: (nodes: readonly VerraCityNodeMeta[]) => void;
  readonly updateCityNodeLevel: (name: string, level: number) => void;
  readonly toggleLayer: (layer: keyof LayerToggles, enabled?: boolean) => void;
  readonly resetLayers: () => void;
  readonly setCameraViewport: (patch: Partial<CameraViewport>) => void;
  readonly setCameraMode: (mode: CameraMode) => void;
  readonly setConnectionState: (
    connectionState: StreamConnectionState,
    error?: StreamProblemDetails | null,
  ) => void;
  readonly setPickedCoordinate: (point: VerraUnrealPoint | null) => void;
  readonly setSpatialOverlays: (
    navmesh: readonly NavmeshLinkSegment[],
  ) => void;
}

export interface WorldStreamStoreApi {
  readonly getState: () => WorldStreamState;
  readonly getInitialState: () => WorldStreamState;
  readonly setState: (
    partial:
      | Partial<WorldStreamState>
      | ((state: WorldStreamState) => Partial<WorldStreamState>),
  ) => void;
  readonly subscribe: (
    listener: (state: WorldStreamState, prevState: WorldStreamState) => void,
  ) => () => void;
}

export interface UseWorldStreamStoreHook extends WorldStreamStoreApi {
  <U = WorldStreamState>(selector?: (state: WorldStreamState) => U): U;
}

export function worldCmToMosaicPx(xCm: number, yCm: number): MosaicPxTuple {
  const u = (xCm / VERRA_MAP_SOURCE_CM - VERRA_MAP_X0) * VERRA_MAP_SOURCE_PX;
  const v = (yCm / VERRA_MAP_SOURCE_CM - VERRA_MAP_Y0) * VERRA_MAP_SOURCE_PX;
  return [u, v];
}

export function mosaicPxToWorldCm(u: number, v: number): WorldCmTuple {
  const xCm = (u / VERRA_MAP_SOURCE_PX + VERRA_MAP_X0) * VERRA_MAP_SOURCE_CM;
  const yCm = (v / VERRA_MAP_SOURCE_PX + VERRA_MAP_Y0) * VERRA_MAP_SOURCE_CM;
  return [xCm, yCm];
}

export function nodeLevelToPlugTier(level: number): number {
  if (!Number.isFinite(level)) {
    return -1;
  }
  const clamped = Math.max(0, Math.min(6, Math.trunc(level)));
  return NODE_PLUG_TIER_BY_LEVEL[clamped] ?? -1;
}

export interface BaseTileDescriptor {
  readonly z: number;
  readonly x: number;
  readonly y: number;
  readonly tileUrl: string;
  readonly mosaicU0: number;
  readonly mosaicV0: number;
  readonly mosaicSpanPx: number;
}

export interface NodePlugTileOverlayDescriptor {
  readonly nodeIndex: number;
  readonly name: string;
  readonly displayName: string;
  readonly level: number;
  readonly plugTier: number;
  readonly tileUrl: string;
  readonly centerMosaicU: number;
  readonly centerMosaicV: number;
  readonly sizeMosaicPx: number;
  readonly sizeCm: number;
  readonly yawDeg: number;
  readonly rotationRad: number;
}

export function computeNodePlugTileOverlay(
  node: VerraCityNodeMeta,
): NodePlugTileOverlayDescriptor | null {
  const plugTier = nodeLevelToPlugTier(node.level);
  if (plugTier < 0) return null;
  const [u, v] = worldCmToMosaicPx(node.ox, node.oy);
  return {
    nodeIndex: node.node_index,
    name: node.name,
    displayName: node.display_name,
    level: node.level,
    plugTier,
    tileUrl: `/api/v1/world/tiles/${VERRA_MAP_PLUG_ZOOM}/${node.node_index}/${plugTier}`,
    centerMosaicU: u,
    centerMosaicV: v,
    sizeMosaicPx: VERRA_NODE_PLUG_SIZE_PX,
    sizeCm: node.size_cm || VERRA_NODE_PLUG_SIZE_CM,
    yawDeg: node.yaw_deg,
    rotationRad: (node.yaw_deg * Math.PI) / 180.0,
  };
}

export function computeVisibleBaseTiles(
  meta: VerraMapMeta,
  centerXCm: number,
  centerYCm: number,
  zoom: number,
  viewportSize: { readonly width: number; readonly height: number },
): readonly BaseTileDescriptor[] {
  const highResZoom = zoom >= 5.0 ? 7 : 6;
  const z = Math.max(6, Math.min(meta.max_zoom, highResZoom));
  const spanPx = meta.tile_px * Math.pow(2, meta.max_zoom - z);
  const scale = Math.pow(2, zoom - 5);
  const [centerU, centerV] = worldCmToMosaicPx(centerXCm, centerYCm);
  const halfSpanU = Math.max(256, viewportSize.width / Math.max(0.015, scale) / 2);
  const halfSpanV = Math.max(256, viewportSize.height / Math.max(0.015, scale) / 2);
  const maxTx = Math.max(0, Math.ceil(meta.width_px / spanPx) - 1);
  const maxTy = Math.max(0, Math.ceil(meta.height_px / spanPx) - 1);
  const tx0 = Math.max(0, Math.floor((centerU - halfSpanU) / spanPx));
  const tx1 = Math.min(maxTx, Math.floor((centerU + halfSpanU) / spanPx));
  const ty0 = Math.max(0, Math.floor((centerV - halfSpanV) / spanPx));
  const ty1 = Math.min(maxTy, Math.floor((centerV + halfSpanV) / spanPx));
  const out: BaseTileDescriptor[] = [];
  for (let ty = ty0; ty <= ty1; ty += 1) {
    for (let tx = tx0; tx <= tx1; tx += 1) {
      if (out.length >= 512) break;
      out.push({ z, x: tx, y: ty, tileUrl: `/api/v1/world/tiles/${z}/${tx}/${ty}`, mosaicU0: tx * spanPx, mosaicV0: ty * spanPx, mosaicSpanPx: spanPx });
    }
  }
  return out;
}

function clampFloat64(value: number, min: number, max: number): number {
  if (!Number.isFinite(value)) {
    return 0.0;
  }
  return Math.min(max, Math.max(min, value));
}

export function verraUnrealToLngLat(xCm: number, yCm: number): LngLatTuple {
  const clampedX = clampFloat64(xCm, -VERRA_EXTENT_CM, VERRA_EXTENT_CM);
  const clampedY = clampFloat64(yCm, -VERRA_EXTENT_CM, VERRA_EXTENT_CM);
  const lng = (clampedX / VERRA_EXTENT_CM) * VERRA_MAX_LNG_DEG;
  const lat = (clampedY / VERRA_EXTENT_CM) * VERRA_MAX_LAT_DEG;
  return [lng, lat];
}

export function lngLatToVerraUnreal(
  lng: number,
  lat: number,
  zCm = 0.0,
): VerraUnrealPoint {
  const clampedLng = clampFloat64(lng, -VERRA_MAX_LNG_DEG, VERRA_MAX_LNG_DEG);
  const clampedLat = clampFloat64(lat, -VERRA_MAX_LAT_DEG, VERRA_MAX_LAT_DEG);
  const safeZ = Number.isFinite(zCm) ? zCm : 0.0;
  return {
    xCm: (clampedLng / VERRA_MAX_LNG_DEG) * VERRA_EXTENT_CM,
    yCm: (clampedLat / VERRA_MAX_LAT_DEG) * VERRA_EXTENT_CM,
    zCm: safeZ,
  };
}

function makeCityNode(
  node_index: number,
  guid: string,
  name: string,
  display_name: string,
  coordsAndLevel: readonly [number, number, number, number],
): VerraCityNodeMeta {
  const [ox, oy, yaw_deg, level] = coordsAndLevel;
  return {
    node_index,
    guid,
    name,
    display_name,
    ox,
    oy,
    yaw_deg,
    size_cm: VERRA_NODE_PLUG_SIZE_CM,
    level,
    plug_tier: nodeLevelToPlugTier(level),
  };
}

export const DEFAULT_VERRA_CITY_NODES: readonly VerraCityNodeMeta[] = Object.freeze([
  makeCityNode(0, "108646108518008", "Verra_RVR_Winstead", "Winstead", [-641389.0, 377367.1, 319.207763671875, 3]),
  makeCityNode(1, "108646121231993", "Verra_RVR_Joeva", "Joeva", [-1092010.8, 522023.2, 292.6263427734375, 3]),
  makeCityNode(2, "108646608033403", "Verra_RVR_NewAela", "New Aela", [-1134183.54, 853387.28, -90.21496, 3]),
  makeCityNode(3, "108646610851452", "Verra_RVR_Halcyon", "Halcyon", [-892130.8, 297711.6, 180.0, 3]),
  makeCityNode(4, "108646615701117", "Verra_RVR_Miraleth", "Miraleth", [-459889.63, 599317.09, -20.0, 3]),
  makeCityNode(5, "6064632225073725498", "Verra_TUR_Windansea", "Windansea", [-1100624.18, 246466.99, 55.41214, 0]),
  makeCityNode(6, "6064632734251679746", "Verra_SAN_Aithanahr", "Aithanahr", [-232975.5, 758608.9, 304.123535, 0]),
  makeCityNode(7, "6064632734253907971", "Verra_SAN_Azmaran", "Azmaran", [-276070.8, 987508.0, 351.156006, 0]),
  makeCityNode(8, "6064632734268915716", "Verra_SAN_Sunhaven", "Sunhaven", [-619803.28, 873514.22, -127.073107, 0]),
  makeCityNode(9, "6064633097318170624", "Verra_SAN_Djinna", "Djinna", [63370.91, 861198.37, -187.137862, 0]),
  makeCityNode(10, "6064633097371058177", "Verra_SAN_SquallsEnd", "Squall's End", [-37370.0, 1133335.0, -217.228668, 0]),
  makeCityNode(11, "6064633334652600320", "Verra_TUR_Korrin", "Korrin", [-1764281.28, 114557.65, 90.707388, 0]),
  makeCityNode(12, "6064633334741794817", "Verra_TUR_Brinebarrel", "Brinebarrel", [-1860980.22, -171832.95, -46.566062, 0]),
  makeCityNode(13, "6064633407845105664", "Verra_TUR_Seahook", "Seahook", [-1764263.89, 539283.28, 14.228906, 0]),
  makeCityNode(14, "6064633407846088705", "Verra_TUR_Shorefoot", "Shorefoot", [-1372642.27, 57374.67, -66.900158, 0]),
  makeCityNode(15, "6064633425419042816", "Verra_JUN_Wildport", "Wildport", [-1784521.8, 811519.5, 16.001587, 0]),
  makeCityNode(16, "6064633425517019141", "Verra_JUN_Hecribba", "Hecribba", [-1707968.6, 1053249.0, 246.000366, 0]),
  makeCityNode(17, "6064633425555816457", "Verra_JUN_Vinebreach", "Vinebreach", [-1476480.0, 1178456.0, 200.0, 0]),
  makeCityNode(18, "6064633429404090381", "Verra_JUN_Arisalon", "Arisalon", [-1250506.0, 1062377.0, -35.0, 0]),
  makeCityNode(19, "6064633429423882257", "Verra_JUN_TangledPost", "Tangled Post", [-1031419.0, 1098848.0, -3.5, 0]),
  makeCityNode(20, "6064633429437251605", "Verra_JUN_Mythbreak", "Mythbreak", [-742705.0, 1087738.0, -159.058146, 0]),
  makeCityNode(21, "6064633651969589265", "Verra_ANV_Vexhelm", "Vexhelm", [-1463138.3, -338058.1, 60.001831, 0]),
  makeCityNode(22, "6064633651985711129", "Verra_ANV_Duunhold", "Duunhold", [-1069784.0, -150970.0, 0.0, 0]),
  makeCityNode(23, "6064633652009173023", "Verra_ANV_Dhurgrum", "Dhurgrum", [-937374.0, -411205.0, -15.0, 0]),
  makeCityNode(24, "6064633652023394340", "Verra_ANV_Vhalgadim", "Vhalgadim", [-762862.0, -563881.0, 169.999997, 0]),
  makeCityNode(25, "6064633652044300331", "Verra_ANV_KalTorhum", "Kal Torhum", [-1274462.4, -462791.5, 4.998779, 0]),
]);

export const DEFAULT_VERRA_MAP_META: VerraMapMeta = Object.freeze({
  x0: VERRA_MAP_X0,
  y0: VERRA_MAP_Y0,
  cols: VERRA_MAP_COLS,
  rows: VERRA_MAP_ROWS,
  source_cm: VERRA_MAP_SOURCE_CM,
  source_px: VERRA_MAP_SOURCE_PX,
  tile_px: VERRA_MAP_TILE_PX,
  width_px: VERRA_MAP_WIDTH_PX,
  height_px: VERRA_MAP_HEIGHT_PX,
  max_zoom: VERRA_MAP_MAX_ZOOM,
  plug_zoom: VERRA_MAP_PLUG_ZOOM,
  nodes: DEFAULT_VERRA_CITY_NODES,
});

export const DEFAULT_LAYER_TOGGLES: LayerToggles = Object.freeze({
  players: true,
  npcs: true,
  monsters: true,
  resource_nodes: true,
  mounts: true,
  loot_piles: true,
  pois: true,
  cityNodes: true,
  nodePlugs: true,
  spawns: true,
  navmesh: false,
});

export function computeStreamLayerMask(
  layers: Partial<Pick<LayerToggles, OverlayLayerKey>>,
): number {
  let mask = 0;
  for (const key of OVERLAY_LAYER_KEYS) {
    if (layers[key] === true) {
      mask |= OVERLAY_LAYER_BITMASK[key];
    }
  }
  return mask;
}

export function buildActiveLayersParam(
  layers: Partial<Pick<LayerToggles, OverlayLayerKey>>,
): string {
  const active: string[] = [];
  for (const key of OVERLAY_LAYER_KEYS) {
    if (layers[key] === true) {
      active.push(key);
    }
  }
  return active.join(",");
}

export function shouldPollWorldState(
  layers: Partial<Pick<LayerToggles, OverlayLayerKey>>,
): boolean {
  return computeStreamLayerMask(layers) > 0;
}

export function buildWorldStatePollUrl(
  layers: Partial<Pick<LayerToggles, OverlayLayerKey>>,
): string | null {
  const layerMask = computeStreamLayerMask(layers);
  const param = buildActiveLayersParam(layers);
  if (layerMask === 0 || param.length === 0) {
    return null;
  }
  return `/api/v1/world/state?layer_mask=${layerMask}&layers=${param}`;
}

export function shouldFetchPoisCatalog(
  layers: Partial<Pick<LayerToggles, "pois">>,
): boolean {
  return layers.pois === true;
}

function pruneHiddenLayerEntities(
  entities: ReadonlyMap<string, WorldEntity>,
  layers: LayerToggles,
): Map<string, WorldEntity> {
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

function computeNextEntityMap(
  current: ReadonlyMap<string, WorldEntity>,
  delta: WorldDeltaBatch,
): Map<string, WorldEntity> {
  const next = delta.isSnapshot ? new Map<string, WorldEntity>() : new Map(current);
  if (delta.removed) {
    for (const guid of delta.removed) {
      next.delete(guid);
    }
  }
  if (delta.added) {
    for (const entity of delta.added) {
      next.set(entity.guid, entity);
    }
  }
  if (delta.updated) {
    for (const entity of delta.updated) {
      const prev = next.get(entity.guid);
      next.set(
        entity.guid,
        prev && !entity.name ? { ...entity, name: prev.name } : entity,
      );
    }
  }
  return next;
}

function computeDeltaPatch(
  state: WorldStreamState,
  delta: WorldDeltaBatch,
): Partial<WorldStreamState> | null {
  if (!Number.isFinite(delta.seq) || delta.seq < 0) {
    return null;
  }
  if (!delta.isSnapshot && state.lastSeq > 0 && delta.seq <= state.lastSeq) {
    return null;
  }
  const hasSeqGap =
    !delta.isSnapshot && state.lastSeq > 0 && delta.seq > state.lastSeq + 1;
  const nextEntities = computeNextEntityMap(state.entities, delta);
  const nextSelected =
    state.selectedEntityGuid && nextEntities.has(state.selectedEntityGuid)
      ? state.selectedEntityGuid
      : null;
  const latency =
    typeof delta.tickLatencyMs === "number" && Number.isFinite(delta.tickLatencyMs)
      ? Math.max(0, delta.tickLatencyMs)
      : state.tickLatencyMs;
  const isOverBudget = latency > UI_FRAME_BUDGET_MS * 2;
  const nextConn: StreamConnectionState =
    hasSeqGap || isOverBudget ? "degraded" : "live";

  return {
    entities: nextEntities,
    selectedEntityGuid: nextSelected,
    lastSeq: delta.seq,
    tickLatencyMs: latency,
    needsSnapshotResync: delta.isSnapshot ? false : hasSeqGap,
    connectionState:
      state.connectionState === "replaying" ? "replaying" : nextConn,
    error: delta.isSnapshot ? null : state.error,
  };
}

function updateNodeLevelInList(
  nodes: readonly VerraCityNodeMeta[],
  name: string,
  level: number,
): readonly VerraCityNodeMeta[] {
  const clampedLevel = Math.max(0, Math.min(6, Math.trunc(level)));
  const plugTier = nodeLevelToPlugTier(clampedLevel);
  return nodes.map((node) =>
    node.name === name || node.display_name === name
      ? { ...node, level: clampedLevel, plug_tier: plugTier }
      : node,
  );
}

function buildInitialWorldStreamState(
  setState: WorldStreamStoreApi["setState"],
  getState: () => WorldStreamState,
  overrides?: Partial<WorldStreamState>,
): WorldStreamState {
  return {
    entities: new Map<string, WorldEntity>(),
    cityNodes: DEFAULT_VERRA_CITY_NODES,
    selectedEntityGuid: null,
    selectedCityNodeName: null,
    selectedPlayer: "",
    activeTab: "map",
    layers: DEFAULT_LAYER_TOGGLES,
    camera: DEFAULT_CAMERA_VIEWPORT,
    connectionState: "connecting",
    lastSeq: 0,
    tickLatencyMs: 0,
    streamHz: STREAM_TARGET_HZ,
    needsSnapshotResync: false,
    error: null,
    pickedCoordinate: null,
    navmeshSegments: [],
    applyWorldDelta: (delta) => {
      const patch = computeDeltaPatch(getState(), delta);
      if (patch) setState(patch);
    },
    selectEntity: (guid) => setState({ selectedEntityGuid: guid }),
    selectCityNode: (name) => setState({ selectedCityNodeName: name }),
    setSelectedPlayer: (selectedPlayer) => setState({ selectedPlayer }),
    setActiveTab: (activeTab) => setState({ activeTab }),
    setCityNodes: (cityNodes) => setState({ cityNodes }),
    updateCityNodeLevel: (name, level) =>
      setState((prev) => ({
        cityNodes: updateNodeLevelInList(prev.cityNodes, name, level),
      })),
    toggleLayer: (layer, enabled) =>
      setState((prev) => {
        const nextVal = enabled !== undefined ? enabled : !prev.layers[layer];
        const nextLayers: LayerToggles = {
          ...prev.layers,
          [layer]: nextVal,
          ...(layer === "pois" ? { cityNodes: nextVal } : {}),
          ...(layer === "cityNodes" ? { pois: nextVal } : {}),
        };
        return {
          layers: nextLayers,
          entities: pruneHiddenLayerEntities(prev.entities, nextLayers),
        };
      }),
    resetLayers: () => setState({ layers: DEFAULT_LAYER_TOGGLES }),
    setCameraViewport: (patch) =>
      setState((prev) => ({ camera: { ...prev.camera, ...patch } })),
    setCameraMode: (mode) =>
      setState((prev) => ({
        camera: {
          ...prev.camera,
          mode,
          pitch: mode === "3d" ? Math.max(prev.camera.pitch, 50.0) : 0.0,
        },
      })),
    setConnectionState: (connectionState, error = null) =>
      setState({ connectionState, error }),
    setPickedCoordinate: (pickedCoordinate) => setState({ pickedCoordinate }),
    setSpatialOverlays: (navmeshSegments) => setState({ navmeshSegments }),
    ...overrides,
  };
}

export function createWorldStreamStore(
  initialOverrides?: Partial<WorldStreamState>,
): WorldStreamStoreApi {
  const listeners = new Set<
    (state: WorldStreamState, prevState: WorldStreamState) => void
  >();
  let state: WorldStreamState;

  const setState: WorldStreamStoreApi["setState"] = (partial) => {
    const patch = typeof partial === "function" ? partial(state) : partial;
    const nextState = Object.assign({}, state, patch);
    if (nextState === state) {
      return;
    }
    const prevState = state;
    state = nextState;
    for (const listener of listeners) {
      listener(state, prevState);
    }
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
  function useWorldStreamStoreSelector<U = WorldStreamState>(
    selector?: (state: WorldStreamState) => U,
  ): U {
    const useSync = (
      React as unknown as {
        useSyncExternalStore?: <T>(
          sub: (cb: () => void) => () => void,
          getSnap: () => T,
        ) => T;
      }
    ).useSyncExternalStore;
    const snap =
      typeof useSync === "function"
        ? useSync(
            defaultWorldStreamStore.subscribe,
            defaultWorldStreamStore.getState,
          )
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
