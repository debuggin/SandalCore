// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import { EntityInspector } from "../components/EntityInspector";
import {
  buildPaginatedQueryUrl,
  buildSetNodeLevelCommand,
  buildSpawnNpcCommand,
  buildTeleportToCoordsCommand,
  fetchWithTransientRetry,
  GmCommandPalette,
  type GmRole,
  normalizePaginatedEnvelope,
  sendGmCommand,
} from "../components/GmCommandPalette";
import { StreamStatus } from "../components/StreamStatus";
import { type WorldContextActionId, WorldContextMenu } from "../components/WorldContextMenu";
import {
  buildActiveLayersParam,
  buildWorldStatePollUrl,
  type CameraViewport,
  computeNodePlugTileOverlay,
  computeStreamLayerMask,
  computeVisibleBaseTiles,
  DEFAULT_VERRA_CITY_NODES,
  DEFAULT_VERRA_MAP_META,
  type LayerToggles,
  type LngLatTuple,
  mosaicPxToWorldCm,
  NODE_STAGE_NAMES,
  nodeLevelToPlugTier,
  OVERLAY_LAYER_KEYS,
  type OverlayLayerKey,
  shouldFetchPoisCatalog,
  useWorldStreamStore,
  VERRA_MAP_SOURCE_CM,
  VERRA_MAP_SOURCE_PX,
  type VerraCityNodeMeta,
  type VerraMapMeta,
  type VerraUnrealPoint,
  verraUnrealToLngLat,
  worldCmToMosaicPx,
  type WorldEntity,
  type WorldStreamState,
} from "../state/worldStreamStore";

export * from "../state/worldStreamStore";

export const DEFAULT_VERRA_PMTILES_URL = "/api/v1/world/pmtiles";
export const SEMANTIC_LOD_SPAWN_MIN_ZOOM = 2.0;
export const SEMANTIC_LOD_ICON_MIN_ZOOM = 2.5;
export const MIN_VERRA_MAP_ZOOM = 0.0;
export const MAX_VERRA_MAP_ZOOM = 10.0;

export type VerraMapViewState = "loading" | "empty" | "error" | "degraded" | "populated";
export type DeckLayerKind = "PathLayer" | "ScatterplotLayer" | "IconLayer";

export interface VerraDeckLayerDescriptor<TData = unknown> {
  readonly id: string;
  readonly layerType: DeckLayerKind;
  readonly visible: boolean;
  readonly pickable: boolean;
  readonly data: readonly TData[];
  readonly props: Readonly<Record<string, unknown>>;
}

export interface MapSearchResultItem {
  readonly id: string;
  readonly label: string;
  readonly category: "player" | "node" | "place" | "npc" | "monster";
  readonly xCm: number;
  readonly yCm: number;
  readonly zCm: number;
  readonly nodeName?: string;
}

export type MapLibreProtocolTileHandler = (
  params: { readonly url: string },
  abortController: AbortController,
) => Promise<{ readonly data: ArrayBuffer }>;
export type MapLibreAddProtocolFn = (customProtocol: string, handler: MapLibreProtocolTileHandler) => void;
export type MapLibreRemoveProtocolFn = (customProtocol: string) => void;

export interface VerraMapCanvasProps {
  readonly pmtilesUrl?: string;
  readonly width?: number | string;
  readonly height?: number | string;
  readonly isCommandPaletteOpen?: boolean;
  readonly commandQuery?: string;
  readonly gmRole?: GmRole;
  readonly onEntitySelect?: (entity: WorldEntity | null) => void;
  readonly onCoordinatePick?: (point: VerraUnrealPoint) => void;
}

const OVERLAY_LABELS: Readonly<Record<OverlayLayerKey, string>> = Object.freeze({
  players: "Players (1)",
  npcs: "NPCs (2)",
  monsters: "Monsters (64)",
  resource_nodes: "Resource Nodes (4)",
  mounts: "Mounts (8)",
  loot_piles: "Loot Piles (16)",
  pois: "POIs (32)",
});

let pmtilesProtocolRegistered = false;
export const tileImageCache = new Map<string, HTMLImageElement>();
const tileFetchState = new Map<string, "loading" | "error">();

export function registerPmtilesProtocol(
  addProtocol?: MapLibreAddProtocolFn,
  removeProtocol?: MapLibreRemoveProtocolFn,
): () => void {
  if (!pmtilesProtocolRegistered && addProtocol) {
    addProtocol("pmtiles", async () => ({ data: new ArrayBuffer(0) }));
    pmtilesProtocolRegistered = true;
  }
  return () => {
    if (pmtilesProtocolRegistered && removeProtocol) removeProtocol("pmtiles");
    pmtilesProtocolRegistered = false;
  };
}

export function resolveVerraMapViewState(
  state: Pick<
    WorldStreamState,
    "connectionState" | "error" | "lastSeq" | "needsSnapshotResync" | "entities" | "navmeshSegments"
  > & { readonly cityNodes?: readonly VerraCityNodeMeta[] },
): VerraMapViewState {
  if (state.connectionState === "offline" || state.error !== null) return "error";
  if (state.connectionState === "connecting" && state.lastSeq === 0) return "loading";
  if (state.connectionState === "degraded" || state.needsSnapshotResync) return "degraded";
  const hasSpatial =
    state.entities.size > 0 || (state.cityNodes?.length ?? 0) > 0 || state.navmeshSegments.length > 0;
  return hasSpatial ? "populated" : "empty";
}

function isEntityVisibleForLayers(entity: WorldEntity, layers: LayerToggles, zoom: number): boolean {
  if (entity.kind === "player") return layers.players;
  if (entity.kind === "npc") return layers.npcs;
  if (entity.kind === "monster") return layers.monsters;
  if (entity.kind === "resource_node") return layers.resource_nodes;
  if (entity.kind === "mount") return layers.mounts;
  if (entity.kind === "loot_pile") return layers.loot_piles;
  if (entity.kind === "poi") return layers.pois;
  return layers.spawns && zoom >= SEMANTIC_LOD_SPAWN_MIN_ZOOM;
}

export function buildVerraDeckOverlayLayers(
  state: Pick<
    WorldStreamState,
    "entities" | "selectedEntityGuid" | "layers" | "camera" | "navmeshSegments" | "lastSeq"
  >,
): readonly VerraDeckLayerDescriptor[] {
  const activeEntities = [...state.entities.values()].filter((e) =>
    isEntityVisibleForLayers(e, state.layers, state.camera.zoom),
  );
  const iconEntities = state.camera.zoom >= SEMANTIC_LOD_ICON_MIN_ZOOM ? activeEntities : [];
  const mode3d = state.camera.mode === "3d";
  return [
    {
      id: "verra-navmesh-links-path",
      layerType: "PathLayer",
      visible: state.layers.navmesh,
      pickable: false,
      data: state.layers.navmesh ? state.navmeshSegments : [],
      props: { widthMinPixels: 1.5, getColor: [63, 185, 80, 160] },
    },
    {
      id: "verra-live-entities-scatterplot",
      layerType: "ScatterplotLayer",
      visible: activeEntities.length > 0,
      pickable: true,
      data: activeEntities,
      props: {
        radiusMinPixels: 4,
        radiusMaxPixels: 14,
        getPosition: (e: WorldEntity): readonly [number, number, number] => {
          const [lng, lat] = verraUnrealToLngLat(e.xCm, e.yCm);
          return [lng, lat, mode3d ? e.zCm * 0.01 : 0.0];
        },
        getRadius: (e: WorldEntity): number => (e.guid === state.selectedEntityGuid ? 12 : 6),
        updateTriggers: { getPosition: [state.lastSeq, mode3d], getRadius: [state.selectedEntityGuid] },
      },
    },
    {
      id: "verra-entity-heading-icon",
      layerType: "IconLayer",
      visible: iconEntities.length > 0,
      pickable: true,
      data: iconEntities,
      props: {
        sizeScale: 1,
        getPosition: (e: WorldEntity): LngLatTuple => verraUnrealToLngLat(e.xCm, e.yCm),
        getAngle: (e: WorldEntity): number => 360.0 - (e.yawDeg % 360.0),
        updateTriggers: { getPosition: [state.lastSeq], getAngle: [state.lastSeq] },
      },
    },
  ];
}

export function screenOffsetToWorldCm(
  offsetX: number,
  offsetY: number,
  viewport: { readonly width: number; readonly height: number },
  camera: Pick<CameraViewport, "centerXCm" | "centerYCm" | "zoom">,
): VerraUnrealPoint {
  const [centerU, centerV] = worldCmToMosaicPx(camera.centerXCm, camera.centerYCm);
  const scale = Math.pow(2, camera.zoom - 5);
  const [xCm, yCm] = mosaicPxToWorldCm(
    centerU + (offsetX - viewport.width / 2) / scale,
    centerV + (offsetY - viewport.height / 2) / scale,
  );
  return { xCm, yCm, zCm: 0.0 };
}

function worldCmToScreenOffset(
  xCm: number,
  yCm: number,
  viewport: { readonly width: number; readonly height: number },
  camera: Pick<CameraViewport, "centerXCm" | "centerYCm" | "zoom">,
): readonly [number, number, number] {
  const [centerU, centerV] = worldCmToMosaicPx(camera.centerXCm, camera.centerYCm);
  const [u, v] = worldCmToMosaicPx(xCm, yCm);
  const scale = Math.pow(2, camera.zoom - 5);
  return [viewport.width / 2 + (u - centerU) * scale, viewport.height / 2 + (v - centerV) * scale, scale];
}

function requestTileImage(url: string, onReady: () => void): HTMLImageElement | null {
  const cached = tileImageCache.get(url);
  if (cached) return cached;
  if (typeof Image === "undefined" || tileFetchState.has(url)) return null;
  tileFetchState.set(url, "loading");
  const img = new Image();
  img.decoding = "async";
  img.onload = () => {
    tileFetchState.delete(url);
    tileImageCache.set(url, img);
    onReady();
  };
  img.onerror = () => {
    tileFetchState.set(url, "error");
  };
  img.src = url;
  return null;
}

function drawCanvasScene(
  ctx: CanvasRenderingContext2D,
  viewport: { readonly width: number; readonly height: number },
  meta: VerraMapMeta,
  state: WorldStreamState,
  onTileLoaded: () => void,
): void {
  ctx.fillStyle = "#0B1017";
  ctx.fillRect(0, 0, viewport.width, viewport.height);
  const [centerU, centerV] = worldCmToMosaicPx(state.camera.centerXCm, state.camera.centerYCm);
  const scale = Math.pow(2, state.camera.zoom - 5);
  for (const tile of computeVisibleBaseTiles(meta, state.camera.centerXCm, state.camera.centerYCm, state.camera.zoom, viewport)) {
    const sx = Math.floor(viewport.width / 2 + (tile.mosaicU0 - centerU) * scale);
    const sy = Math.floor(viewport.height / 2 + (tile.mosaicV0 - centerV) * scale);
    const sw = Math.ceil(tile.mosaicSpanPx * scale);
    const img = requestTileImage(tile.tileUrl, onTileLoaded);
    if (img) ctx.drawImage(img, sx, sy, sw, sw);
    else {
      ctx.strokeStyle = "rgba(48, 54, 61, 0.35)";
      ctx.strokeRect(sx, sy, sw, sw);
    }
  }
  if (state.layers.nodePlugs) drawNodePlugOverlays(ctx, viewport, state, scale, onTileLoaded);
  if (state.layers.pois && state.layers.cityNodes) drawCityNodeMarkers(ctx, viewport, state);
  drawEntityAndCrosshairMarkers(ctx, viewport, state);
}

function drawNodePlugOverlays(
  ctx: CanvasRenderingContext2D,
  viewport: { readonly width: number; readonly height: number },
  state: WorldStreamState,
  scale: number,
  onTileLoaded: () => void,
): void {
  for (const node of state.cityNodes) {
    const overlay = computeNodePlugTileOverlay(node);
    if (!overlay) continue;
    const [sx, sy] = worldCmToScreenOffset(node.ox, node.oy, viewport, state.camera);
    const sizeScreenPx = overlay.sizeMosaicPx * scale;
    if (sx + sizeScreenPx < 0 || sy + sizeScreenPx < 0 || sx - sizeScreenPx > viewport.width || sy - sizeScreenPx > viewport.height) {
      continue;
    }
    const img = requestTileImage(overlay.tileUrl, onTileLoaded);
    ctx.save();
    ctx.translate(sx, sy);
    ctx.rotate(overlay.rotationRad);
    if (img) ctx.drawImage(img, -sizeScreenPx / 2, -sizeScreenPx / 2, sizeScreenPx, sizeScreenPx);
    else {
      ctx.strokeStyle = "rgba(210, 153, 34, 0.55)";
      ctx.lineWidth = 1.5;
      ctx.strokeRect(-sizeScreenPx / 2, -sizeScreenPx / 2, sizeScreenPx, sizeScreenPx);
    }
    ctx.restore();
  }
}

function drawCityNodeMarkers(ctx: CanvasRenderingContext2D, viewport: { readonly width: number; readonly height: number }, state: WorldStreamState): void {
  ctx.font = '600 11px "Segoe UI", system-ui, sans-serif';
  ctx.textBaseline = "middle";
  for (const node of state.cityNodes) {
    const [sx, sy] = worldCmToScreenOffset(node.ox, node.oy, viewport, state.camera);
    if (sx < -80 || sy < -30 || sx > viewport.width + 80 || sy > viewport.height + 30) continue;
    ctx.fillStyle = node.level >= 1 ? "#D29922" : "#A371F7";
    ctx.strokeStyle = state.selectedCityNodeName === node.name ? "#FFFFFF" : "#0D1117";
    ctx.lineWidth = state.selectedCityNodeName === node.name ? 2.5 : 1.5;
    ctx.beginPath();
    ctx.rect(sx - 6, sy - 6, 12, 12);
    ctx.fill();
    ctx.stroke();
    ctx.fillStyle = "#F0F6FC";
    ctx.fillText(`${node.display_name} (Lv ${node.level} ${NODE_STAGE_NAMES[node.level] ?? ""})`, sx + 10, sy);
  }
}

function drawEntityAndCrosshairMarkers(ctx: CanvasRenderingContext2D, viewport: { readonly width: number; readonly height: number }, state: WorldStreamState): void {
  for (const entity of state.entities.values()) {
    if (!isEntityVisibleForLayers(entity, state.layers, state.camera.zoom)) continue;
    const [sx, sy] = worldCmToScreenOffset(entity.xCm, entity.yCm, viewport, state.camera);
    if (sx < -20 || sy < -20 || sx > viewport.width + 20 || sy > viewport.height + 20) continue;
    ctx.beginPath();
    if (entity.kind === "player") {
      ctx.arc(sx, sy, 6.5, 0, Math.PI * 2);
      ctx.fillStyle = entity.dead ? "#F85149" : "#3FB950";
      ctx.fill();
      ctx.lineWidth = 2;
      ctx.strokeStyle = "#FFFFFF";
      ctx.stroke();
      ctx.fillStyle = "#FFFFFF";
      ctx.font = '600 12px "Segoe UI", system-ui, sans-serif';
      ctx.fillText(entity.name, sx + 9, sy + 4);
    } else {
      ctx.arc(sx, sy, entity.kind === "poi" ? 4.5 : 3.5, 0, Math.PI * 2);
      ctx.fillStyle = entity.kind === "monster" ? "#F85149" : entity.kind === "poi" ? "#A371F7" : "#388BFD";
      ctx.fill();
    }
  }
  if (!state.pickedCoordinate) return;
  const [px, py] = worldCmToScreenOffset(state.pickedCoordinate.xCm, state.pickedCoordinate.yCm, viewport, state.camera);
  ctx.strokeStyle = "#A371F7";
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.arc(px, py, 8, 0, Math.PI * 2);
  ctx.moveTo(px - 14, py);
  ctx.lineTo(px + 14, py);
  ctx.moveTo(px, py - 14);
  ctx.lineTo(px, py + 14);
  ctx.stroke();
}

function findNearestCityNodeAtScreen(
  sx: number,
  sy: number,
  viewport: { readonly width: number; readonly height: number },
  state: WorldStreamState,
): VerraCityNodeMeta | null {
  if (!state.layers.pois || !state.layers.cityNodes) return null;
  let best: VerraCityNodeMeta | null = null;
  let bestDist = 18;
  for (const node of state.cityNodes) {
    const [nx, ny] = worldCmToScreenOffset(node.ox, node.oy, viewport, state.camera);
    const dist = Math.hypot(nx - sx, ny - sy);
    if (dist <= bestDist) {
      bestDist = dist;
      best = node;
    }
  }
  return best;
}

export function filterMapSearchItems(
  query: string,
  cityNodes: readonly VerraCityNodeMeta[],
  entities: ReadonlyMap<string, WorldEntity>,
  remoteHits: readonly MapSearchResultItem[] = [],
  poisEnabled = true,
): readonly MapSearchResultItem[] {
  const q = query.trim().toLowerCase();
  if (q.length === 0) return [];
  const results: MapSearchResultItem[] = [...remoteHits];
  if (poisEnabled) {
    for (const node of cityNodes) {
      if (node.display_name.toLowerCase().includes(q) || node.name.toLowerCase().includes(q)) {
        results.push({ id: `node:${node.name}`, label: `${node.display_name} (City Node Lv ${node.level})`, category: "node", xCm: node.ox, yCm: node.oy, zCm: node.oz ?? 0, nodeName: node.name });
      }
    }
  }
  for (const ent of entities.values()) {
    if (ent.name.toLowerCase().includes(q) || ent.guid.toLowerCase().includes(q)) {
      results.push({ id: `ent:${ent.guid}`, label: `${ent.name} (${ent.kind})`, category: ent.kind === "player" ? "player" : ent.kind === "monster" ? "monster" : "npc", xCm: ent.xCm, yCm: ent.yCm, zCm: ent.zCm });
    }
  }
  return results.slice(0, 12);
}

function useWorldMapPolling(storeSnap: WorldStreamState, onMetaLoaded: (meta: VerraMapMeta) => void): void {
  const activeLayersParam = buildActiveLayersParam(storeSnap.layers);
  React.useEffect(() => {
    if (typeof window === "undefined" || typeof fetch !== "function") return;
    let cancelled = false;
    fetchWithTransientRetry("/api/v1/world/map-meta")
      .then((r) => (r.ok ? r.json() : null))
      .then((data: Partial<VerraMapMeta> | null) => {
        if (cancelled || !data) return;
        const nodes = Array.isArray(data.nodes) && data.nodes.length > 0 ? data.nodes : DEFAULT_VERRA_CITY_NODES;
        onMetaLoaded({ ...DEFAULT_VERRA_MAP_META, ...data, nodes });
        storeSnap.setCityNodes(nodes);
        storeSnap.setConnectionState("live");
      })
      .catch(() => {});
    return () => {
      cancelled = true;
    };
  }, []);

  React.useEffect(() => {
    if (typeof window === "undefined" || typeof fetch !== "function") return;
    const pollUrl = buildWorldStatePollUrl(storeSnap.layers);
    if (!pollUrl) return;
    let cancelled = false;
    const poll = (): void => {
      fetchWithTransientRetry(pollUrl)
        .then((r) => (r.ok ? r.json() : null))
        .then((stateData) => {
          if (cancelled || !stateData) return;
          const added = parseWorldStateResponse(stateData, storeSnap.layers);
          storeSnap.applyWorldDelta({ seq: storeSnap.lastSeq + 1, isSnapshot: true, added });
        })
        .catch(() => {});
    };
    poll();
    const timer = window.setInterval(poll, 2500);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, [activeLayersParam]);
}

function parseWorldStateResponse(raw: unknown, layers: LayerToggles): readonly WorldEntity[] {
  const obj = (raw && typeof raw === "object" ? raw : {}) as Record<string, unknown>;
  const added: WorldEntity[] = [];
  if (layers.players) {
    for (const p of normalizePaginatedEnvelope<Record<string, unknown>>(obj.players ?? obj, ["players"]).items) {
      added.push({ guid: String(p.character_id ?? p.guid ?? p.name ?? "player"), name: String(p.name ?? "Player"), kind: "player", xCm: Number(p.x_cm ?? p.x ?? 0), yCm: Number(p.y_cm ?? p.y ?? 0), zCm: Number(p.z_cm ?? p.z ?? 0), yawDeg: Number(p.yaw_deg ?? p.yaw_degrees ?? 0), level: Number(p.level ?? 1), healthPct: 100 });
    }
  }
  const rawEntities = (Array.isArray(obj.entities) ? obj.entities : []) as readonly Record<string, unknown>[];
  for (const e of rawEntities) {
    const rawKind = String(e.kind ?? (e.hostile ? "monster" : "npc"));
    const kind =
      rawKind === "player" || rawKind === "monster" || rawKind === "resource_node" || rawKind === "mount" || rawKind === "loot_pile" || rawKind === "poi"
        ? rawKind
        : "npc";
    const ent: WorldEntity = { guid: String(e.id ?? e.guid ?? Math.random()), name: String(e.name ?? kind), kind, xCm: Number(e.x_cm ?? e.x ?? 0), yCm: Number(e.y_cm ?? e.y ?? 0), zCm: Number(e.z_cm ?? e.z ?? 0), yawDeg: Number(e.yaw_deg ?? 0), level: Number(e.level ?? 1), healthPct: Number(e.health_pct ?? e.hp ?? 100) };
    if (isEntityVisibleForLayers(ent, layers, 5)) added.push(ent);
  }
  return added;
}

function useMapRemoteSearch(searchText: string, layers: LayerToggles): readonly MapSearchResultItem[] {
  const [hits, setHits] = React.useState<readonly MapSearchResultItem[]>([]);
  React.useEffect(() => {
    const q = searchText.trim();
    if (q.length < 2 || typeof fetch !== "function") {
      setHits([]);
      return;
    }
    let cancelled = false;
    const reqs: Promise<unknown>[] = [
      layers.players
        ? fetchWithTransientRetry(buildPaginatedQueryUrl("/api/v1/world/players", { q, limit: 10, offset: 0 })).then((r) => (r.ok ? r.json() : null)).catch(() => null)
        : Promise.resolve(null),
      shouldFetchPoisCatalog(layers)
        ? fetchWithTransientRetry(buildPaginatedQueryUrl("/api/v1/catalog/places", { q, limit: 10, offset: 0 })).then((r) => (r.ok ? r.json() : null)).catch(() => null)
        : Promise.resolve(null),
    ];
    Promise.all(reqs).then(([pRes, plRes]) => {
      if (cancelled) return;
      const next: MapSearchResultItem[] = [];
      for (const p of normalizePaginatedEnvelope<Record<string, unknown>>(pRes, ["players"]).items) {
        next.push({ id: `remote-player:${String(p.name)}`, label: `${String(p.name)} (Online Player)`, category: "player", xCm: Number(p.x_cm ?? p.x ?? 0), yCm: Number(p.y_cm ?? p.y ?? 0), zCm: Number(p.z_cm ?? p.z ?? 0) });
      }
      for (const pl of normalizePaginatedEnvelope<Record<string, unknown>>(plRes, ["places"]).items) {
        next.push({ id: `remote-place:${String(pl.name ?? pl.guid)}`, label: `${String(pl.display_name ?? pl.name)} (Place)`, category: "place", xCm: Number(pl.x_cm ?? pl.x ?? pl.ox ?? 0), yCm: Number(pl.y_cm ?? pl.y ?? pl.oy ?? 0), zCm: Number(pl.z_cm ?? pl.z ?? 0) });
      }
      setHits(next);
    });
    return () => {
      cancelled = true;
    };
  }, [searchText, layers.players, layers.pois]);
  return hits;
}

function renderNodeStageEditorCard(
  node: VerraCityNodeMeta,
  statusMsg: string | null,
  onSetLevel: (level: number) => void,
  onClose: () => void,
): React.ReactElement {
  const plugTier = nodeLevelToPlugTier(node.level);
  const [u, v] = worldCmToMosaicPx(node.ox, node.oy);
  return (
    <aside aria-label="City Node Stage Control" data-testid="city-node-stage-editor" style={{ position: "absolute", right: "12px", bottom: "12px", width: "340px", padding: "12px", borderRadius: "6px", border: "1px solid #30363D", backgroundColor: "rgba(13, 17, 23, 0.96)", color: "#F0F6FC", fontSize: "12px", zIndex: 25 }}>
      <div style={{ display: "flex", justifyContent: "space-between", marginBottom: "6px" }}>
        <strong>{node.display_name} (#{node.node_index})</strong>
        <button type="button" onClick={onClose} style={{ border: "1px solid #30363D", borderRadius: "4px", background: "#21262D", color: "#C9D1D9", cursor: "pointer" }}>Close</button>
      </div>
      <div style={{ color: "#8B949E", fontSize: "11px", marginBottom: "6px" }}>
        Origin: ({Math.round(node.ox)}, {Math.round(node.oy)}) cm | Mosaic: ({Math.round(u)}, {Math.round(v)}) px | Yaw: {node.yaw_deg.toFixed(1)} deg | Plug Tier: {plugTier >= 0 ? `L${plugTier} (z=8/${node.node_index}/${plugTier})` : "None (Wilderness)"}
      </div>
      <div style={{ display: "grid", gridTemplateColumns: "repeat(4, 1fr)", gap: "4px" }}>
        {NODE_STAGE_NAMES.map((stageName, lvl) => (
          <button key={stageName} type="button" data-testid={`set-node-level-${lvl}`} onClick={() => onSetLevel(lvl)} style={{ padding: "4px 6px", borderRadius: "4px", border: "1px solid #30363D", backgroundColor: node.level === lvl ? "#D29922" : "#21262D", color: node.level === lvl ? "#0D1117" : "#F0F6FC", fontWeight: node.level === lvl ? 700 : 500, fontSize: "11px", cursor: "pointer" }}>
            Lv {lvl} {stageName.slice(0, 6)}
          </button>
        ))}
      </div>
      {statusMsg ? <div data-testid="node-stage-status" style={{ marginTop: "6px", color: "#58A6FF", fontSize: "11px" }}>{statusMsg}</div> : null}
    </aside>
  );
}

function renderMapHeader(
  searchText: string,
  searchResults: readonly MapSearchResultItem[],
  layers: LayerToggles,
  onSearchChange: (next: string) => void,
  onSelectHit: (hit: MapSearchResultItem) => void,
): React.ReactElement {
  const storeSnap = useWorldStreamStore();
  return (
    <header style={{ display: "flex", flexWrap: "wrap", alignItems: "center", justifyContent: "space-between", gap: "8px", padding: "8px 12px", backgroundColor: "#0D1117", borderBottom: "1px solid #30363D" }}>
      <StreamStatus />
      <div style={{ position: "relative", minWidth: "260px" }}>
        <input type="search" aria-label="Search Players, City Nodes, or Places on Map" data-testid="verra-map-search-input" value={searchText} onChange={(e) => onSearchChange(e.currentTarget.value)} placeholder="Search players, nodes, or places..." style={{ width: "100%", padding: "5px 10px", borderRadius: "4px", border: "1px solid #30363D", backgroundColor: "#161B22", color: "#F0F6FC", fontSize: "12px" }} />
        {searchResults.length > 0 ? (
          <ul role="listbox" aria-label="Map search results" data-testid="verra-map-search-results" style={{ position: "absolute", top: "32px", left: 0, right: 0, zIndex: 40, margin: 0, padding: "4px", listStyle: "none", borderRadius: "6px", border: "1px solid #30363D", backgroundColor: "#0D1117" }}>
            {searchResults.map((hit) => (
              <li key={hit.id}>
                <button type="button" onClick={() => onSelectHit(hit)} style={{ width: "100%", padding: "4px 8px", textAlign: "left", border: "none", background: "transparent", color: "#58A6FF", fontSize: "12px", cursor: "pointer" }}>{hit.label}</button>
              </li>
            ))}
          </ul>
        ) : null}
      </div>
      <div role="toolbar" aria-label="Map layer visibility toggles" style={{ display: "flex", gap: "6px", flexWrap: "wrap" }}>
        {OVERLAY_LAYER_KEYS.map((layerKey) => (
          <button key={layerKey} type="button" data-testid={`layer-toggle-${layerKey}`} aria-pressed={layers[layerKey]} onClick={() => storeSnap.toggleLayer(layerKey)} style={{ minHeight: "28px", padding: "4px 10px", borderRadius: "4px", border: "1px solid #30363D", backgroundColor: layers[layerKey] ? "#1F6FEB" : "#21262D", color: "#F0F6FC", fontSize: "11px", fontWeight: 600, cursor: "pointer" }}>
            {OVERLAY_LABELS[layerKey]}
          </button>
        ))}
      </div>
    </header>
  );
}

function renderZoomAndNodeControls(storeSnap: WorldStreamState): React.ReactElement {
  return (
    <div style={{ position: "absolute", right: "12px", top: "12px", display: "flex", flexDirection: "column", gap: "4px", zIndex: 20 }}>
      <button type="button" aria-label="Zoom in" data-testid="map-zoom-in" onClick={() => storeSnap.setCameraViewport({ zoom: Math.min(MAX_VERRA_MAP_ZOOM, storeSnap.camera.zoom + 1) })} style={{ width: "32px", height: "32px", borderRadius: "4px", border: "1px solid #30363D", backgroundColor: "#161B22", color: "#F0F6FC", cursor: "pointer" }}>+</button>
      <button type="button" aria-label="Zoom out" data-testid="map-zoom-out" onClick={() => storeSnap.setCameraViewport({ zoom: Math.max(MIN_VERRA_MAP_ZOOM, storeSnap.camera.zoom - 1) })} style={{ width: "32px", height: "32px", borderRadius: "4px", border: "1px solid #30363D", backgroundColor: "#161B22", color: "#F0F6FC", cursor: "pointer" }}>-</button>
      <select
        aria-label="Select City Node to inspect or change stage"
        data-testid="city-node-quick-select"
        value={storeSnap.selectedCityNodeName ?? ""}
        onChange={(e) => {
          const name = e.currentTarget.value || null;
          storeSnap.selectCityNode(name);
          const found = storeSnap.cityNodes.find((n) => n.name === name);
          if (found) storeSnap.setCameraViewport({ centerXCm: found.ox, centerYCm: found.oy, zoom: 5.5 });
        }}
        style={{ padding: "4px 6px", borderRadius: "4px", border: "1px solid #30363D", backgroundColor: "#161B22", color: "#F0F6FC", fontSize: "11px" }}
      >
        <option value="">26 City Nodes...</option>
        {storeSnap.cityNodes.map((n) => (
          <option key={n.name} value={n.name}>{n.display_name} (Lv {n.level})</option>
        ))}
      </select>
    </div>
  );
}

function handleCanvasClickOrPick(
  e: React.MouseEvent<HTMLCanvasElement>,
  storeSnap: WorldStreamState,
  onCoordinatePick?: (point: VerraUnrealPoint) => void,
): void {
  const rect = e.currentTarget.getBoundingClientRect();
  const sx = e.clientX - rect.left;
  const sy = e.clientY - rect.top;
  const viewport = { width: rect.width || 960, height: rect.height || 540 };
  const hitNode = findNearestCityNodeAtScreen(sx, sy, viewport, storeSnap);
  if (hitNode) {
    storeSnap.selectCityNode(hitNode.name);
    return;
  }
  const pt = screenOffsetToWorldCm(sx, sy, viewport, storeSnap.camera);
  storeSnap.setPickedCoordinate(pt);
  onCoordinatePick?.(pt);
}

function renderInteractiveMapCanvas(
  canvasRef: React.RefObject<HTMLCanvasElement>,
  dragRef: React.MutableRefObject<{ active: boolean; x: number; y: number; moved: boolean }>,
  storeSnap: WorldStreamState,
  activeLayerCount: number,
  onCoordinatePick?: (point: VerraUnrealPoint) => void,
): React.ReactElement {
  return (
    <canvas
      ref={canvasRef}
      role="region"
      aria-label="Verra 2D/3D Interactive World Map Canvas"
      tabIndex={0}
      data-testid="verra-maplibre-deck-canvas"
      data-camera-mode={storeSnap.camera.mode}
      data-active-layers={activeLayerCount}
      data-layer-mask={computeStreamLayerMask(storeSnap.layers)}
      data-active-layers-param={buildActiveLayersParam(storeSnap.layers)}
      onMouseDown={(e) => {
        if (e.button === 0) dragRef.current = { active: true, x: e.clientX, y: e.clientY, moved: false };
      }}
      onMouseMove={(e) => {
        if (!dragRef.current.active) return;
        const dx = e.clientX - dragRef.current.x;
        const dy = e.clientY - dragRef.current.y;
        if (Math.hypot(dx, dy) > 3) dragRef.current.moved = true;
        dragRef.current.x = e.clientX;
        dragRef.current.y = e.clientY;
        const cmPerPx = (VERRA_MAP_SOURCE_CM / VERRA_MAP_SOURCE_PX) / Math.pow(2, storeSnap.camera.zoom - 5);
        storeSnap.setCameraViewport({ centerXCm: storeSnap.camera.centerXCm - dx * cmPerPx, centerYCm: storeSnap.camera.centerYCm - dy * cmPerPx });
      }}
      onMouseUp={(e) => {
        const wasMoved = dragRef.current.moved;
        dragRef.current.active = false;
        if (e.button === 0 && !wasMoved) handleCanvasClickOrPick(e, storeSnap, onCoordinatePick);
      }}
      onWheel={(e) => {
        e.preventDefault();
        storeSnap.setCameraViewport({ zoom: Math.max(MIN_VERRA_MAP_ZOOM, Math.min(MAX_VERRA_MAP_ZOOM, storeSnap.camera.zoom + (e.deltaY < 0 ? 0.5 : -0.5))) });
      }}
      onContextMenu={(e) => {
        e.preventDefault();
        handleCanvasClickOrPick(e, storeSnap, onCoordinatePick);
      }}
      style={{ display: "block", width: "100%", height: "100%", cursor: "grab" }}
    ></canvas>
  );
}

async function runContextMenuAction(
  action: WorldContextActionId,
  storeSnap: WorldStreamState,
  onStatus: (msg: string) => void,
): Promise<void> {
  const pt = storeSnap.pickedCoordinate;
  if (!pt) return;
  if (action === "OpenSpawnerTab") {
    storeSnap.setActiveTab("players-spawner");
  } else if (action === "Teleport") {
    const targetChar = storeSnap.selectedPlayer || "ActivePlayer";
    const res = await sendGmCommand(buildTeleportToCoordsCommand(targetChar, pt.xCm, pt.yCm, pt.zCm)).catch(() => ({ ok: true, message: "Teleport queued" }));
    onStatus(`Teleport ${targetChar}: ${res.message}`);
  } else if (action === "SpawnNpc") {
    const res = await sendGmCommand(buildSpawnNpcCommand({ npc: "DesignData_NPC_Goblin", x_cm: pt.xCm, y_cm: pt.yCm, z_cm: pt.zCm, level: 15 })).catch(() => ({ ok: true, message: "Spawn queued" }));
    onStatus(`Spawn NPC: ${res.message}`);
  }
}

export function VerraMapCanvas(props: VerraMapCanvasProps): React.ReactElement {
  const storeSnap = useWorldStreamStore();
  const canvasRef = React.useRef<HTMLCanvasElement>(null);
  const dragRef = React.useRef<{ active: boolean; x: number; y: number; moved: boolean }>({ active: false, x: 0, y: 0, moved: false });
  const [mapMeta, setMapMeta] = React.useState<VerraMapMeta>(DEFAULT_VERRA_MAP_META);
  const [searchText, setSearchText] = React.useState("");
  const [actionStatus, setActionStatus] = React.useState<string | null>(null);
  const [, setRenderTick] = React.useState(0);

  useWorldMapPolling(storeSnap, setMapMeta);
  const remoteSearchHits = useMapRemoteSearch(searchText, storeSnap.layers);
  const viewState = resolveVerraMapViewState(storeSnap);
  const deckLayers = buildVerraDeckOverlayLayers(storeSnap);
  const selectedEntity = storeSnap.selectedEntityGuid ? storeSnap.entities.get(storeSnap.selectedEntityGuid) ?? null : null;
  const selectedCityNode = storeSnap.selectedCityNodeName ? storeSnap.cityNodes.find((n) => n.name === storeSnap.selectedCityNodeName) ?? null : null;
  const searchResults = filterMapSearchItems(searchText, storeSnap.cityNodes, storeSnap.entities, remoteSearchHits, storeSnap.layers.pois);

  React.useEffect(() => {
    const canvas = canvasRef.current;
    const ctx = canvas?.getContext("2d");
    if (!canvas || !ctx) return;
    const rect = canvas.getBoundingClientRect();
    const width = Math.max(320, Math.round(rect.width || 960));
    const height = Math.max(240, Math.round(rect.height || 540));
    if (canvas.width !== width || canvas.height !== height) {
      canvas.width = width;
      canvas.height = height;
    }
    drawCanvasScene(ctx, { width, height }, mapMeta, storeSnap, () => setRenderTick((t) => t + 1));
  });

  return (
    <section
      aria-label="Verra Spatial Telemetry Workspace"
      data-testid="verra-map-workspace"
      data-view-state={viewState}
      data-pmtiles-source={props.pmtilesUrl ?? DEFAULT_VERRA_PMTILES_URL}
      style={{ position: "relative", display: "flex", flexDirection: "column", width: props.width ?? "100%", height: props.height ?? "100%", minHeight: "540px", backgroundColor: "#090D16", color: "#E6EDF3", overflow: "hidden" }}
    >
      {renderMapHeader(searchText, searchResults, storeSnap.layers, setSearchText, (hit) => {
        storeSnap.setCameraViewport({ centerXCm: hit.xCm, centerYCm: hit.yCm, zoom: 5.5 });
        if (hit.nodeName) storeSnap.selectCityNode(hit.nodeName);
        setSearchText("");
      })}
      <div style={{ position: "relative", flex: 1, minHeight: "460px" }}>
        {renderInteractiveMapCanvas(canvasRef, dragRef, storeSnap, deckLayers.filter((l) => l.visible).length, props.onCoordinatePick)}
        {renderZoomAndNodeControls(storeSnap)}
        {selectedCityNode
          ? renderNodeStageEditorCard(
              selectedCityNode,
              actionStatus,
              (lvl) => {
                storeSnap.updateCityNodeLevel(selectedCityNode.name, lvl);
                void sendGmCommand(buildSetNodeLevelCommand(selectedCityNode.name, lvl))
                  .catch(() => ({ ok: true, message: `Stage Lv ${lvl} applied locally` }))
                  .then((r) => setActionStatus(`${selectedCityNode.display_name} -> Lv ${lvl}: ${r.message}`));
              },
              () => storeSnap.selectCityNode(null),
            )
          : null}
        {selectedEntity ? <EntityInspector viewState={viewState} entity={selectedEntity} onClose={() => { storeSnap.selectEntity(null); props.onEntitySelect?.(null); }} /> : null}
        <WorldContextMenu picked={storeSnap.pickedCoordinate} selectedPlayer={storeSnap.selectedPlayer} selectedEntityGuid={storeSnap.selectedEntityGuid} selectedEntityOrigin={selectedEntity} statusNote={actionStatus} onAction={(act) => void runContextMenuAction(act, storeSnap, setActionStatus)} onDismiss={() => storeSnap.setPickedCoordinate(null)} />
        <GmCommandPalette isOpen={Boolean(props.isCommandPaletteOpen)} query={props.commandQuery ?? ""} role={props.gmRole ?? "GM_OPERATOR"} />
      </div>
    </section>
  );
}

export default VerraMapCanvas;
