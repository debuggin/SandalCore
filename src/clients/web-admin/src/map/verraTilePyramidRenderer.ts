// SPDX-License-Identifier: AGPL-3.0-only

import { normalizePaginatedEnvelope } from "../components/GmCommandPalette";
import {
  appendAdminAccessTokenToUrl, CIRCUIT_FAILURE_THRESHOLD, CIRCUIT_OPEN_COOLDOWN_MS, computeExponentialBackoffMs,
  isUnauthenticatedViewOnly, recordRetryFailure, recordRetrySuccess, registerPendingRetryTimer,
  RETRY_BASE_DELAY_MS, RETRY_MAX_ATTEMPTS, RETRY_MAX_DELAY_MS, subscribeRetryReset,
} from "../state/adminAuthClient";
import { clipZoiRingToValidMinimapTiles, hydrateVerraZoiPolygonsFromTsvApi, VERRA_ZOI_POLYGONS, type VerraZoiPolygon } from "./verraZoiPolygons";
export { clipZoiRingToValidMinimapTiles, hydrateVerraZoiPolygonsFromTsvApi, VERRA_ZOI_POLYGONS, type VerraZoiPolygon };
import {
  type CameraViewport, computeNodePlugTileOverlay, computeVisibleBaseTiles, DEFAULT_CAMERA_VIEWPORT, DEFAULT_VERRA_MAP_META, getCityNodeCenterCm,
  type LayerEntityCounts, type LayerToggles, type LngLatTuple, mosaicPxToWorldCm, type OverlayLayerKey,
  type PoiOverlay, type VerraCityNodeMeta, type VerraMapMeta, type VerraUnrealPoint, verraUnrealToLngLat, worldCmToMosaicPx,
  type WorldEntity, type WorldStreamState,
} from "../state/worldStreamStore";

export const DEFAULT_VERRA_PMTILES_URL = "/api/v1/world/pmtiles";
export const SEMANTIC_LOD_SPAWN_MIN_ZOOM = 2.0;
export const SEMANTIC_LOD_ICON_MIN_ZOOM = 2.5;
export const MIN_VERRA_MAP_ZOOM = 0.0;
export const MAX_VERRA_MAP_ZOOM = 10.0;
export const PURE_BLACK_TILE_BG = "#000000";

export type VerraMapViewState = "loading" | "empty" | "error" | "degraded" | "populated";
export type DeckLayerKind = "PathLayer" | "ScatterplotLayer" | "IconLayer";

export interface VerraDeckLayerDescriptor<TData = unknown> {
  readonly id: string; readonly layerType: DeckLayerKind; readonly visible: boolean;
  readonly pickable: boolean; readonly data: readonly TData[]; readonly props: Readonly<Record<string, unknown>>;
}

export interface MapSearchResultItem {
  readonly id: string; readonly label: string;
  readonly category: "player" | "node" | "place" | "npc" | "monster" | "poi" | "resource_node";
  readonly xCm: number; readonly yCm: number; readonly zCm: number;
  readonly playerName?: string; readonly nodeName?: string; readonly entityGuid?: string; readonly poiGuid?: string;
}

export type MapLibreProtocolTileHandler = (params: { readonly url: string }, abortController: AbortController) => Promise<{ readonly data: ArrayBuffer }>;
export type MapLibreAddProtocolFn = (customProtocol: string, handler: MapLibreProtocolTileHandler) => void;
export type MapLibreRemoveProtocolFn = (customProtocol: string) => void;

interface TileCacheEntry {
  readonly key: string; readonly url: string; state: "queued" | "loading" | "ready" | "missing" | "error";
  img: HTMLImageElement | null; used: number; dist: number; retries: number; retryAfterMs: number; pinned: boolean;
}

export interface AncestorTileFallback {
  readonly img: HTMLImageElement; readonly sx: number; readonly sy: number; readonly sw: number; readonly sh: number; readonly ancestorZ: number;
}

export const tileImageCache = new Map<string, HTMLImageElement>();
const tileFetchState = new Map<string, "loading" | "error">();

export class VerraTileCache {
  private readonly map = new Map<string, TileCacheEntry>();
  private readonly queue: TileCacheEntry[] = [];
  private readonly retryTimers = new Set<ReturnType<typeof setTimeout>>();
  private readonly onLoaded: () => void;
  private readonly maxEntries: number;
  public readonly maxInflight: number;
  private inflight = 0;
  private frame = 0;
  private preloaded = false;
  private lastTokenUrl = "";
  private consecutiveFailures = 0;
  private circuitOpenUntilMs = 0;

  constructor(onLoaded: () => void, maxEntries = 2048, maxInflight = 32) {
    this.onLoaded = onLoaded;
    this.maxEntries = maxEntries;
    this.maxInflight = maxInflight;
    subscribeRetryReset(() => this.resetAllRetries());
  }

  beginFrame(): void {
    this.frame += 1;
    const curTokenUrl = appendAdminAccessTokenToUrl("/t");
    if (curTokenUrl !== this.lastTokenUrl) {
      this.lastTokenUrl = curTokenUrl;
      this.resetAllRetries();
    }
  }

  resetAllRetries(): void {
    for (const timer of this.retryTimers) clearTimeout(timer);
    this.retryTimers.clear();
    this.consecutiveFailures = 0;
    this.circuitOpenUntilMs = 0;
    tileFetchState.clear();
    for (const entry of this.map.values()) {
      if (entry.state === "error" || entry.state === "missing") {
        entry.state = "queued";
        entry.retries = 0;
        entry.retryAfterMs = 0;
        this.queue.push(entry);
      }
    }
    this.pump();
    this.onLoaded();
  }

  resetFailedEntries(): void {
    this.resetAllRetries();
  }

  notifyLoaded(): void {
    this.onLoaded();
  }

  getFrame(): number {
    return this.frame;
  }

  preloadCoarseLevels(meta: VerraMapMeta = DEFAULT_VERRA_MAP_META, maxCoarseZ = 2): void {
    if (this.preloaded) return;
    this.preloaded = true;
    for (let z = 0; z <= Math.min(maxCoarseZ, meta.max_zoom); z += 1) {
      const span = meta.tile_px * Math.pow(2, meta.max_zoom - z);
      const cols = Math.ceil(meta.width_px / span), rows = Math.ceil(meta.height_px / span);
      for (let y = 0; y < rows; y += 1) {
        for (let x = 0; x < cols; x += 1) this.enqueueTile(z, x, y, 0, true);
      }
    }
    this.pump();
  }

  getTile(z: number, x: number, y: number, dist = 0): HTMLImageElement | null {
    const entry = this.enqueueTile(z, x, y, dist, z <= 2);
    this.pump();
    return entry.state === "ready" ? entry.img : null;
  }

  peekTile(z: number, x: number, y: number): HTMLImageElement | null {
    const entry = this.map.get(`${z}/${x}/${y}`);
    if (!entry) return null;
    entry.used = this.frame;
    return entry.state === "ready" ? entry.img : null;
  }

  findAncestorFallback(z: number, tx: number, ty: number, tilePx = 256): AncestorTileFallback | null {
    for (let dz = 1; dz <= z; dz += 1) {
      const upZ = z - dz, factor = 1 << dz, upX = tx >> dz, upY = ty >> dz;
      const upImg = this.peekTile(upZ, upX, upY) ?? this.getTile(upZ, upX, upY, 1e6 + dz);
      if (upImg) {
        const part = tilePx / factor;
        return { img: upImg, sx: (tx % factor) * part, sy: (ty % factor) * part, sw: part, sh: part, ancestorZ: upZ };
      }
    }
    return null;
  }

  private enqueueTile(z: number, x: number, y: number, dist: number, pinned: boolean): TileCacheEntry {
    const key = `${z}/${x}/${y}`, url = `/api/v1/world/tiles/${z}/${x}/${y}`;
    let entry = this.map.get(key);
    if (!entry) {
      entry = { key, url, state: "queued", img: null, used: this.frame, dist, retries: 0, retryAfterMs: 0, pinned };
      this.map.set(key, entry);
      this.queue.push(entry);
      this.evictIfNeeded();
    } else {
      entry.used = this.frame;
      entry.dist = dist;
      if (pinned) entry.pinned = true;
      if ((entry.state === "error" || entry.state === "missing") && Date.now() >= entry.retryAfterMs) {
        entry.state = "queued";
        entry.retries = 0;
        this.queue.push(entry);
      }
    }
    return entry;
  }

  private handleTileError(entry: TileCacheEntry): void {
    this.inflight = Math.max(0, this.inflight - 1);
    this.consecutiveFailures += 1;
    recordRetryFailure();
    if (this.consecutiveFailures >= CIRCUIT_FAILURE_THRESHOLD) {
      this.circuitOpenUntilMs = Date.now() + CIRCUIT_OPEN_COOLDOWN_MS;
    }
    if (entry.retries < RETRY_MAX_ATTEMPTS) {
      const backoffMs = computeExponentialBackoffMs(entry.retries, RETRY_BASE_DELAY_MS, RETRY_MAX_DELAY_MS);
      const delay = Math.max(Math.max(0, this.circuitOpenUntilMs - Date.now()), backoffMs);
      entry.retries += 1;
      entry.state = "error";
      entry.retryAfterMs = Date.now() + delay;
      if (typeof setTimeout === "function") {
        const timer = setTimeout(() => {
          this.retryTimers.delete(timer);
          if (entry.state === "error" || entry.state === "missing") {
            entry.state = "queued";
            this.queue.push(entry);
            this.pump();
          }
        }, delay);
        this.retryTimers.add(timer);
        registerPendingRetryTimer(timer);
      }
    } else {
      entry.state = "missing";
      entry.retryAfterMs = Date.now() + RETRY_MAX_DELAY_MS;
    }
    this.pump();
  }

  private pump(): void {
    if (typeof Image === "undefined") return;
    this.queue.sort((a, b) => a.dist - b.dist);
    while (this.inflight < this.maxInflight && this.queue.length > 0) {
      const entry = this.queue.shift()!;
      if (!entry.pinned && entry.used < this.frame - 600) {
        this.map.delete(entry.key);
        continue;
      }
      if (entry.state !== "queued") continue;
      entry.state = "loading";
      this.inflight += 1;
      const img = new Image();
      img.decoding = "async";
      img.onload = () => {
        entry.img = img;
        entry.state = "ready";
        entry.retries = 0;
        this.consecutiveFailures = 0;
        this.circuitOpenUntilMs = 0;
        recordRetrySuccess();
        tileImageCache.set(entry.url, img);
        this.inflight = Math.max(0, this.inflight - 1);
        this.pump();
        this.onLoaded();
      };
      img.onerror = () => this.handleTileError(entry);
      img.src = entry.url;
    }
  }

  private evictIfNeeded(): void {
    if (this.map.size <= this.maxEntries) return;
    const candidates = [...this.map.values()].filter((e) => !e.pinned && (e.state === "ready" || e.state === "missing")).sort((a, b) => a.used - b.used);
    for (const item of candidates.slice(0, Math.ceil(this.maxEntries * 0.25))) this.map.delete(item.key);
  }
}

export function formatOverlayLayerLabel(layerKey: OverlayLayerKey, counts: LayerEntityCounts): string {
  if (layerKey === "players") return `Players (${counts.players})`;
  if (layerKey === "npcs") return `NPCs (${counts.npcs})`;
  if (layerKey === "monsters") return `Monsters (${counts.monsters})`;
  if (layerKey === "resource_nodes") return `Resources (${counts.resources})`;
  if (layerKey === "mounts") return `Mounts (${counts.mounts})`;
  if (layerKey === "loot_piles") return `Loot Piles (${counts.loot_piles})`;
  return `POIs (${counts.pois})`;
}

let pmtilesProtocolRegistered = false;

export function registerPmtilesProtocol(addProtocol?: MapLibreAddProtocolFn, removeProtocol?: MapLibreRemoveProtocolFn): () => void {
  if (!pmtilesProtocolRegistered && addProtocol) { addProtocol("pmtiles", async () => ({ data: new ArrayBuffer(0) })); pmtilesProtocolRegistered = true; }
  return () => { if (pmtilesProtocolRegistered && removeProtocol) removeProtocol("pmtiles"); pmtilesProtocolRegistered = false; };
}

export function resolveVerraMapViewState(
  state: Pick<WorldStreamState, "connectionState" | "error" | "lastSeq" | "needsSnapshotResync" | "entities" | "navmeshSegments"> & {
    readonly cityNodes?: readonly VerraCityNodeMeta[]; readonly pois?: readonly PoiOverlay[];
  },
): VerraMapViewState {
  if (state.connectionState === "offline" || state.error !== null) return "error";
  if (state.connectionState === "connecting" && state.lastSeq === 0) return "loading";
  if (state.connectionState === "degraded" || state.needsSnapshotResync) return "degraded";
  const hasSpatial = state.entities.size > 0 || (state.cityNodes?.length ?? 0) > 0 || (state.pois?.length ?? 0) > 0 || state.navmeshSegments.length > 0;
  return hasSpatial ? "populated" : "empty";
}

export function isEntityVisibleForLayers(entity: WorldEntity, layers: LayerToggles, zoom: number): boolean {
  if ((entity.npcSubkind ?? entity.npc_subkind) === "emberspring") return layers.pois || layers.npcs;
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
  state: Pick<WorldStreamState, "entities" | "selectedEntityGuid" | "layers" | "camera" | "navmeshSegments" | "lastSeq">,
): readonly VerraDeckLayerDescriptor[] {
  const activeEntities = [...state.entities.values()].filter((e) => isEntityVisibleForLayers(e, state.layers, state.camera.zoom));
  const iconEntities = state.camera.zoom >= SEMANTIC_LOD_ICON_MIN_ZOOM ? activeEntities : [];
  const mode3d = state.camera.mode === "3d";
  return [
    { id: "verra-navmesh-links-path", layerType: "PathLayer", visible: state.layers.navmesh, pickable: false, data: state.layers.navmesh ? state.navmeshSegments : [], props: { widthMinPixels: 1.5, getColor: [34, 197, 94, 160] } },
    {
      id: "verra-live-entities-scatterplot", layerType: "ScatterplotLayer", visible: activeEntities.length > 0, pickable: true, data: activeEntities,
      props: {
        radiusMinPixels: 4, radiusMaxPixels: 14,
        getPosition: (e: WorldEntity): readonly [number, number, number] => { const [lng, lat] = verraUnrealToLngLat(e.xCm, e.yCm); return [lng, lat, mode3d ? e.zCm * 0.01 : 0.0]; },
        getRadius: (e: WorldEntity): number => (e.guid === state.selectedEntityGuid ? 12 : 6),
        updateTriggers: { getPosition: [state.lastSeq, mode3d], getRadius: [state.selectedEntityGuid] },
      },
    },
    {
      id: "verra-entity-heading-icon", layerType: "IconLayer", visible: iconEntities.length > 0, pickable: true, data: iconEntities,
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

export function worldCmToScreenOffset(
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
  img.src = appendAdminAccessTokenToUrl(url);
  return null;
}

const VALID_MINIMAP_TILE_CONTOUR: ReadonlyArray<readonly [number, number]> = Object.freeze([
  [2560, 0], [17408, 0], [17408, 8704], [19456, 8704], [19456, 13824],
  [25600, 13824], [25600, 22016], [14336, 22016], [14336, 23552], [2560, 23552],
]);

export function drawCanvasScene(
  ctx: CanvasRenderingContext2D,
  viewport: { readonly width: number; readonly height: number },
  meta: VerraMapMeta,
  state: WorldStreamState,
  tileCache: VerraTileCache,
): void {
  // Missing or unloaded tiles must remain pure black (#000000) without wireframe boxes.
  ctx.fillStyle = PURE_BLACK_TILE_BG;
  ctx.fillRect(0, 0, viewport.width, viewport.height);
  tileCache.beginFrame();
  tileCache.preloadCoarseLevels(meta, 2);
  const cam = isUnauthenticatedViewOnly() ? DEFAULT_CAMERA_VIEWPORT : state.camera;
  const [centerU, centerV] = worldCmToMosaicPx(cam.centerXCm, cam.centerYCm);
  const scale = Math.pow(2, cam.zoom - 5);
  const visibleTiles = computeVisibleBaseTiles(meta, cam.centerXCm, cam.centerYCm, cam.zoom, viewport);
  for (const tile of visibleTiles) {
    const sx = Math.floor(viewport.width / 2 + (tile.mosaicU0 - centerU) * scale);
    const sy = Math.floor(viewport.height / 2 + (tile.mosaicV0 - centerV) * scale);
    const sw = Math.ceil(tile.mosaicSpanPx * scale);
    const img = tileCache.getTile(tile.z, tile.x, tile.y, tile.distFromCenterPx ?? 0) ?? tileImageCache.get(tile.tileUrl) ?? null;
    if (img) { ctx.drawImage(img, sx, sy, sw, sw); continue; }
    const fallback = tileCache.findAncestorFallback(tile.z, tile.x, tile.y, meta.tile_px);
    if (fallback) ctx.drawImage(fallback.img, fallback.sx, fallback.sy, fallback.sw, fallback.sh, sx, sy, sw, sw);
  }
  if (isUnauthenticatedViewOnly() || (state.connectionState === "offline" && state.cityNodes.length === 0)) return;
  const onAsyncAssetLoaded = () => tileCache.notifyLoaded();
  if (state.layers.nodePlugs) drawNodePlugOverlays(ctx, viewport, state, onAsyncAssetLoaded);
  if (state.layers.zoiBorders) drawZoiBorderOverlays(ctx, viewport, state);
  drawEntityAndCrosshairMarkers(ctx, viewport, state, onAsyncAssetLoaded);
  if (state.layers.pois) drawPoiAndCityNodeMarkers(ctx, viewport, state, onAsyncAssetLoaded);
}

function drawZoiBorderOverlays(
  ctx: CanvasRenderingContext2D,
  viewport: { readonly width: number; readonly height: number },
  state: WorldStreamState,
): void {
  const [centerU, centerV] = worldCmToMosaicPx(state.camera.centerXCm, state.camera.centerYCm);
  const scale = Math.pow(2, state.camera.zoom - 5);
  ctx.save();
  ctx.beginPath();
  for (let i = 0; i < VALID_MINIMAP_TILE_CONTOUR.length; i += 1) {
    const cx = viewport.width / 2 + (VALID_MINIMAP_TILE_CONTOUR[i][0] - centerU) * scale;
    const cy = viewport.height / 2 + (VALID_MINIMAP_TILE_CONTOUR[i][1] - centerV) * scale;
    if (i === 0) ctx.moveTo(cx, cy); else ctx.lineTo(cx, cy);
  }
  ctx.closePath?.();
  ctx.clip?.();
  for (const zoi of VERRA_ZOI_POLYGONS) {
    if (!zoi.has_in_game_border) continue;
    for (const ring of zoi.rings) {
      if (ring.length < 3) continue;
      ctx.beginPath();
      for (let i = 0; i < ring.length; i += 1) {
        const sx = viewport.width / 2 + (ring[i][0] - centerU) * scale, sy = viewport.height / 2 + (ring[i][1] - centerV) * scale;
        if (i === 0) ctx.moveTo(sx, sy); else ctx.lineTo(sx, sy);
      }
      ctx.closePath?.();
      ctx.save(); ctx.clip?.(); ctx.fillStyle = zoi.color || "#E8B25A"; ctx.globalAlpha = 0.06; ctx.fill(); ctx.restore();
      ctx.save(); ctx.setLineDash?.([]); ctx.lineJoin = "round"; ctx.lineCap = "round";
      ctx.strokeStyle = "rgba(6, 9, 15, 0.88)"; ctx.globalAlpha = 0.92; ctx.lineWidth = 4.4; ctx.stroke();
      ctx.strokeStyle = zoi.color || "#E8B25A"; ctx.globalAlpha = 0.38; ctx.lineWidth = 6.0; ctx.stroke();
      ctx.strokeStyle = zoi.color || "#F6CC7E"; ctx.globalAlpha = 0.98; ctx.lineWidth = 2.4; ctx.stroke();
      ctx.restore();
    }
    const isCityNodeZoi = state.layers.cityNodes && (zoi.name.includes("_Node_") || state.cityNodes.some((n) => n.display_name === zoi.display_name || n.name === zoi.name));
    const cx = viewport.width / 2 + (zoi.centroid_u - centerU) * scale, cy = viewport.height / 2 + (zoi.centroid_v - centerV) * scale;
    if (!isCityNodeZoi && cx >= -60 && cy >= -20 && cx <= viewport.width + 60 && cy <= viewport.height + 20 && state.camera.zoom >= 1.8) {
      ctx.save(); ctx.setLineDash?.([]); ctx.font = '700 11px "Segoe UI", system-ui, sans-serif';
      ctx.strokeStyle = "rgba(6, 9, 15, 0.9)"; ctx.lineWidth = 3; ctx.strokeText?.(zoi.display_name, cx - 24, cy);
      ctx.fillStyle = "#F6CC7E"; ctx.fillText(zoi.display_name, cx - 24, cy); ctx.restore();
    }
  }
  ctx.setLineDash?.([]);
  ctx.restore();
}

function drawNodePlugOverlays(
  ctx: CanvasRenderingContext2D,
  viewport: { readonly width: number; readonly height: number },
  state: WorldStreamState,
  onTileLoaded: () => void,
): void {
  for (const node of state.cityNodes) {
    const overlay = computeNodePlugTileOverlay(node, state.camera.zoom);
    if (!overlay) continue;
    const [sx, sy] = worldCmToScreenOffset(overlay.cxCm, overlay.cyCm, viewport, state.camera);
    const sizeScreenPx = overlay.drawSpanPx;
    if (sx + sizeScreenPx < 0 || sy + sizeScreenPx < 0 || sx - sizeScreenPx > viewport.width || sy - sizeScreenPx > viewport.height) continue;
    const img = requestTileImage(overlay.tileUrl, onTileLoaded);
    if (!img) continue;
    // Node plug tiles are full 1024x1024 squares rotated by +yaw_deg (never clip into rings).
    ctx.save();
    ctx.translate(sx, sy);
    ctx.rotate(overlay.rotationRad);
    ctx.drawImage(img, -sizeScreenPx / 2, -sizeScreenPx / 2, sizeScreenPx, sizeScreenPx);
    ctx.restore();
  }
}

export const POI_FALLBACK_ICON_BY_KIND: Readonly<Record<string, string>> = Object.freeze({
  emberspring: "TUI_Icon_Map_ReviveShrine_Active",
  wayshrine: "TUI_Icon_Map_ReviveShrine_Active",
  harbor: "TUI_Icon_Map_Node_Dock",
  castle: "TUI_Icon_Map_Siege",
  starting_area: "TUI_Icon_Map_HomeShrine",
  spawn: "TUI_Icon_Map_HomeShrine",
  outpost: "TUI_Icon_Map_MinorPOI_Discovered",
  city_node: "TUI_Icon_Node_LVL3",
  node_plug: "TUI_Icon_Node_LVL3",
});

export function resolveCityNodeIconName(node: { readonly name?: string; readonly display_name?: string; readonly level?: number; readonly stage?: number; readonly icon_name?: string; readonly iconName?: string }): string {
  const custom = (node.icon_name ?? node.iconName ?? "").trim().replace(/\.png$/i, "");
  if (custom.startsWith("TUI_Icon_")) return custom;
  const lvl = Number.isFinite(node.level) ? Number(node.level) : Number(node.stage ?? 1);
  if (lvl >= 6) return "TUI_Icon_Node_Sovereignty";
  const clamped = Math.min(4, Math.max(0, lvl > 0 ? lvl - 1 : 0));
  return `TUI_Icon_Node_LVL${clamped}`;
}

function isCityNodePoi(poi: PoiOverlay): boolean {
  const k = (poi.kind ?? poi.category ?? "").trim().toLowerCase(), c = (poi.category ?? "").trim().toLowerCase();
  return k === "city_node" || k === "node_plug" || c === "city_node" || c === "node_plug";
}

function measureCleanLabelBox(ctx: CanvasRenderingContext2D, text: string, cx: number, ty: number): readonly [number, number, number, number] | null {
  const cleanText = text.replace(/\s*\((?:Wilderness|Expedition|Encampment|Village|Town|City|Metropolis|Lv\s*\d+)\)\s*$/i, "").trim();
  if (!cleanText) return null;
  ctx.font = '700 11px "Segoe UI", system-ui, sans-serif';
  const halfW = (typeof ctx.measureText === "function" ? ctx.measureText(cleanText).width : cleanText.length * 6.5) / 2;
  return [cx - halfW - 2, ty - 8, cx + halfW + 2, ty + 8];
}

function drawPoiAndCityNodeMarkers(ctx: CanvasRenderingContext2D, viewport: { readonly width: number; readonly height: number }, state: WorldStreamState, onIconLoaded: () => void): void {
  const iconPx = Math.max(18, Math.min(28, Math.round(18 + (state.camera.zoom - 2) * 2)));
  ctx.font = '700 11px "Segoe UI", system-ui, sans-serif';
  ctx.textBaseline = "middle";
  const poiTakenLabels: Array<readonly [number, number, number, number]> = [];
  if (state.layers.cityNodes) {
    for (const node of state.cityNodes) {
      const { cx_cm, cy_cm } = getCityNodeCenterCm(node);
      const [sx, sy] = worldCmToScreenOffset(cx_cm, cy_cm, viewport, state.camera);
      const box = measureCleanLabelBox(ctx, node.display_name || node.name, sx, sy - 21);
      if (box) poiTakenLabels.push(box);
    }
  }
  renderPoiMarkers(ctx, viewport, state, iconPx, poiTakenLabels, onIconLoaded);
  const cityTakenLabels: Array<readonly [number, number, number, number]> = [];
  renderCityNodes(ctx, viewport, state, cityTakenLabels, onIconLoaded);
}

export function renderPoiMarkers(ctx: CanvasRenderingContext2D, viewport: { readonly width: number; readonly height: number }, state: WorldStreamState, iconPx: number, takenLabels: Array<readonly [number, number, number, number]>, onIconLoaded: () => void): void {
  for (const poi of state.pois ?? []) {
    if (isCityNodePoi(poi) || (state.layers.cityNodes && poi.kind !== "wayshrine" && state.cityNodes.some((n) => n.name === poi.name || n.display_name === poi.display_name || n.guid === poi.guid))) continue;
    const [sx, sy] = worldCmToScreenOffset(poi.x_cm, poi.y_cm, viewport, state.camera);
    if (sx < -40 || sy < -30 || sx > viewport.width + 40 || sy > viewport.height + 30) continue;
    const fb = POI_FALLBACK_ICON_BY_KIND[(poi.kind ?? poi.category ?? "").trim().toLowerCase()];
    const iconUrl = poi.icon_url || (fb ? `/api/v1/world/icons/${fb}.png` : "");
    const iconImg = iconUrl ? requestTileImage(iconUrl, onIconLoaded) : null;
    if (iconImg) ctx.drawImage(iconImg, sx - iconPx / 2, sy - iconPx / 2, iconPx, iconPx);
    else { ctx.beginPath(); ctx.arc(sx, sy, 5.5, 0, Math.PI * 2); ctx.fillStyle = poi.tint_color || "#d4af37"; ctx.fill(); }
    if (state.selectedPoiGuid === poi.guid) {
      ctx.strokeStyle = "#d4af37"; ctx.lineWidth = 2.5; ctx.beginPath(); ctx.arc(sx, sy, Math.round(iconPx / 2) + 3, 0, Math.PI * 2); ctx.stroke();
    }
    const isNodeShrineLabel = poi.kind === "wayshrine" && (poi.name.toUpperCase().startsWith("NODE_") || (state.layers.cityNodes && state.cityNodes.some((n) => n.display_name === poi.display_name)));
    if (state.selectedPoiGuid === poi.guid || (!isNodeShrineLabel && state.camera.zoom >= 3.2)) {
      drawCollisionSafeLabel(ctx, poi.display_name || poi.name, sx, sy - Math.round(iconPx / 2) - 7, takenLabels);
    }
  }
}

export function renderCityNodes(ctx: CanvasRenderingContext2D, viewport: { readonly width: number; readonly height: number }, state: WorldStreamState, takenLabels: Array<readonly [number, number, number, number]>, onIconLoaded: () => void): void {
  if (!state.layers.cityNodes) return;
  for (const node of state.cityNodes) {
    const { cx_cm, cy_cm } = getCityNodeCenterCm(node);
    const [sx, sy] = worldCmToScreenOffset(cx_cm, cy_cm, viewport, state.camera);
    if (sx < -80 || sy < -30 || sx > viewport.width + 80 || sy > viewport.height + 30) continue;
    const nodeImg = requestTileImage(`/api/v1/world/icons/${resolveCityNodeIconName(node)}.png`, onIconLoaded);
    if (nodeImg) {
      ctx.beginPath(); ctx.arc(sx, sy, 14, 0, Math.PI * 2); ctx.fillStyle = "rgba(8, 11, 17, 0.78)"; ctx.fill();
      if (state.selectedCityNodeName === node.name) { ctx.strokeStyle = "#ffffff"; ctx.lineWidth = 2; ctx.stroke(); }
      ctx.drawImage(nodeImg, sx - 13, sy - 13, 26, 26);
    } else {
      ctx.fillStyle = node.level >= 1 ? "#d4af37" : "#64748b";
      ctx.strokeStyle = state.selectedCityNodeName === node.name ? "#ffffff" : "#080b11";
      ctx.lineWidth = state.selectedCityNodeName === node.name ? 2.5 : 1.5;
      ctx.beginPath(); ctx.rect(sx - 6, sy - 6, 12, 12); ctx.fill(); ctx.stroke();
    }
    drawCollisionSafeLabel(ctx, node.display_name || node.name, sx, sy - 21, takenLabels);
  }
  for (const poi of state.pois ?? []) {
    if (!isCityNodePoi(poi) || state.cityNodes.some((n) => n.name === poi.name || n.display_name === poi.display_name || n.guid === poi.guid)) continue;
    const [sx, sy] = worldCmToScreenOffset(poi.x_cm, poi.y_cm, viewport, state.camera);
    if (sx < -80 || sy < -30 || sx > viewport.width + 80 || sy > viewport.height + 30) continue;
    const iconName = resolveCityNodeIconName({ name: poi.name, display_name: poi.display_name, level: poi.level, icon_name: poi.icon_name || POI_FALLBACK_ICON_BY_KIND.city_node });
    const nodeImg = requestTileImage(`/api/v1/world/icons/${iconName}.png`, onIconLoaded);
    if (nodeImg) ctx.drawImage(nodeImg, sx - 13, sy - 13, 26, 26);
    else { ctx.fillStyle = "#d4af37"; ctx.beginPath(); ctx.rect(sx - 6, sy - 6, 12, 12); ctx.fill(); }
    drawCollisionSafeLabel(ctx, poi.display_name || poi.name, sx, sy - 21, takenLabels);
  }
}

function drawCollisionSafeLabel(ctx: CanvasRenderingContext2D, text: string, cx: number, ty: number, taken: Array<readonly [number, number, number, number]>): void {
  const cleanText = text.replace(/\s*\((?:Wilderness|Expedition|Encampment|Village|Town|City|Metropolis|Lv\s*\d+)\)\s*$/i, "").trim();
  if (!cleanText) return;
  ctx.font = '700 11px "Segoe UI", system-ui, sans-serif';
  const width = typeof ctx.measureText === "function" ? ctx.measureText(cleanText).width : cleanText.length * 6.5;
  const halfW = width / 2;
  const box: readonly [number, number, number, number] = [cx - halfW - 2, ty - 8, cx + halfW + 2, ty + 8];
  for (const other of taken) {
    if (box[0] < other[2] && box[2] > other[0] && box[1] < other[3] && box[3] > other[1]) return;
  }
  taken.push(box);
  const drawX = Math.round(cx - halfW);
  ctx.save(); ctx.setLineDash?.([]); ctx.font = '700 11px "Segoe UI", system-ui, sans-serif'; ctx.textBaseline = "middle"; ctx.lineJoin = "round";
  ctx.strokeStyle = "rgba(6, 9, 15, 0.92)"; ctx.lineWidth = 3.2; ctx.strokeText?.(cleanText, drawX, ty);
  ctx.fillStyle = "#F6CC7E"; ctx.fillText(cleanText, drawX, ty); ctx.restore();
}

export type WorldPopulationCategory = "players" | "npcs" | "monsters" | "huntables" | "trees" | "rocks" | "ore_gems" | "plants" | "mounts";

export const ENTITY_MAP_LEGEND_SWATCHES = Object.freeze([
  { id: "npcs", label: "NPCs", color: "#38bdf8" },
  { id: "monsters", label: "Monsters", color: "#ef4444" },
  { id: "huntables", label: "Huntables", color: "#f97316" },
  { id: "trees", label: "Trees", color: "#22c55e" },
  { id: "rocks", label: "Rocks", color: "#94a3b8" },
  { id: "ore_gems", label: "Ore/Gems", color: "#eab308" },
  { id: "plants", label: "Plants", color: "#2dd4bf" },
] as const);

export function classifyWorldEntityCategory(
  entity: Pick<WorldEntity, "kind" | "name" | "npc_subkind" | "npcSubkind" | "resourceType" | "resource_type">,
): WorldPopulationCategory {
  if (entity.kind === "player") return "players";
  if (entity.kind === "mount") return "mounts";
  const subkind = (entity.npcSubkind ?? entity.npc_subkind ?? "").toLowerCase();
  const resType = (entity.resourceType ?? entity.resource_type ?? "").toLowerCase();
  const name = (entity.name ?? "").toLowerCase();
  if (subkind === "huntable" || /hunting|husbandry/.test(resType) || /^hunting_/.test(name)) return "huntables";
  if (entity.kind === "resource_node" || resType.length > 0) {
    const combined = `${resType} ${name}`;
    if (/(?:^|[\s_/]|\b)(metal|gem|ore|vein|copper|iron|zinc|tin|rividium|ruby|sapphire|emerald|geode|crystal|halcyonite|lumadon|wyrdstone|resonite|coveglass|obsidian|gold|silver|mythril|cobalt|diamond|amethyst|topaz|quartz)(?:$|[\s_/]|\b)|^(metal|gem)_|_(wyrdstone|resonite|coveglass|obsidian)_/i.test(combined)) return "ore_gems";
    if (/mining\s*\/\s*stone|stonecutting|deadcoral|dead_coral|rock|stone|granite|slate|basalt|limestone|sandstone|marble|boulder/i.test(combined)) return "rocks";
    if (/herbalism|plant|coral|herb|daffodil|moonbell|snowdrop|spiritbloom|flax|yarrow|flower|mushroom|moss|root|berry|banana|birdofparadise|dracaena|elephantear|anemone|bluebell|fern|pross|poppy|salvewort|spindlevine|shrub|bush|leaf|lily|lotus|aloe|sage|thistle/i.test(combined)) return "plants";
    return "trees";
  }
  return entity.kind === "monster" ? "monsters" : "npcs";
}

const CATEGORY_MARKER_COLORS: Readonly<Record<WorldPopulationCategory, string>> = Object.freeze({
  players: "#22c55e", npcs: "#38bdf8", monsters: "#ef4444", huntables: "#f97316",
  trees: "#22c55e", rocks: "#94a3b8", ore_gems: "#eab308", plants: "#2dd4bf", mounts: "#38bdf8",
});

export function resolveEntityMapMarkerColor(
  entity: Pick<WorldEntity, "kind" | "name" | "npc_subkind" | "npcSubkind" | "resourceType" | "resource_type">,
): string {
  return entity.kind === "poi" ? "#d4af37" : CATEGORY_MARKER_COLORS[classifyWorldEntityCategory(entity)];
}

function drawEntityAndCrosshairMarkers(
  ctx: CanvasRenderingContext2D,
  viewport: { readonly width: number; readonly height: number },
  state: WorldStreamState,
  onIconLoaded: () => void = () => {},
): void {
  for (const entity of state.entities.values()) {
    if (!isEntityVisibleForLayers(entity, state.layers, state.camera.zoom)) continue;
    const [sx, sy] = worldCmToScreenOffset(entity.xCm, entity.yCm, viewport, state.camera);
    if (sx < -20 || sy < -20 || sx > viewport.width + 20 || sy > viewport.height + 20) continue;
    ctx.beginPath();
    if (entity.kind === "player") {
      ctx.arc(sx, sy, 6.5, 0, Math.PI * 2);
      ctx.fillStyle = entity.dead ? "#ef4444" : "#22c55e";
      ctx.fill();
      ctx.lineWidth = 2; ctx.strokeStyle = "#ffffff"; ctx.stroke();
      ctx.fillStyle = "#ffffff"; ctx.font = '600 12px "Segoe UI", system-ui, sans-serif';
      ctx.fillText(entity.name, sx + 9, sy + 4);
    } else if ((entity.npcSubkind ?? entity.npc_subkind) === "emberspring") {
      const iconImg = requestTileImage("/api/v1/world/icons/TUI_Icon_Map_ReviveShrine_Active.png", onIconLoaded);
      if (iconImg) ctx.drawImage(iconImg, sx - 9, sy - 9, 18, 18);
      else {
        ctx.moveTo(sx, sy - 6); ctx.lineTo(sx + 6, sy); ctx.lineTo(sx, sy + 6); ctx.lineTo(sx - 6, sy);
        ctx.closePath?.(); ctx.fillStyle = "#22d3ee"; ctx.fill();
      }
    } else {
      ctx.arc(sx, sy, entity.kind === "poi" ? 4.5 : 3.5, 0, Math.PI * 2);
      ctx.fillStyle = resolveEntityMapMarkerColor(entity); ctx.fill();
    }
    if (state.selectedEntityGuid === entity.guid) {
      ctx.strokeStyle = "#d4af37"; ctx.lineWidth = 2.5;
      ctx.beginPath(); ctx.arc(sx, sy, 12, 0, Math.PI * 2); ctx.stroke();
    }
  }
  const pin = state.lastClickedPoint ?? state.pickedCoordinate;
  if (!pin) return;
  const [px, py] = worldCmToScreenOffset(pin.xCm, pin.yCm, viewport, state.camera);
  ctx.strokeStyle = "#d4af37"; ctx.lineWidth = 2;
  ctx.beginPath(); ctx.arc(px, py, 8, 0, Math.PI * 2);
  ctx.moveTo(px - 14, py); ctx.lineTo(px + 14, py); ctx.moveTo(px, py - 14); ctx.lineTo(px, py + 14);
  ctx.stroke();
}

export function handleCanvasRightClickClear(storeSnap: Pick<WorldStreamState, "clearMapSelection">): void {
  storeSnap.clearMapSelection();
}

export function findNearestEntityOrNodeAtScreen(
  sx: number,
  sy: number,
  viewport: { readonly width: number; readonly height: number },
  state: WorldStreamState,
): { readonly entity?: WorldEntity; readonly node?: VerraCityNodeMeta; readonly poi?: PoiOverlay } | null {
  let bestEnt: WorldEntity | null = null, bestEntDist = 18;
  for (const ent of state.entities.values()) {
    if (!isEntityVisibleForLayers(ent, state.layers, state.camera.zoom)) continue;
    const [ex, ey] = worldCmToScreenOffset(ent.xCm, ent.yCm, viewport, state.camera);
    const isShrineOrPoi = (ent.npcSubkind ?? ent.npc_subkind) === "emberspring" || ent.kind === "poi";
    const d = Math.hypot(ex - sx, ey - sy), hitR = isShrineOrPoi || ent.kind === "player" ? 18 : 16;
    if (d <= hitR && (d < bestEntDist || (isShrineOrPoi && bestEnt && (bestEnt.npcSubkind ?? bestEnt.npc_subkind) !== "emberspring" && d <= bestEntDist + 4))) {
      bestEntDist = d; bestEnt = ent;
    }
  }
  let bestPoi: PoiOverlay | null = null, bestPoiDist = 20;
  if (state.layers.pois) {
    for (const poi of state.pois ?? []) {
      if (state.layers.cityNodes && poi.kind === "city_node" && state.cityNodes.some((n) => n.name === poi.name || n.display_name === poi.display_name || n.guid === poi.guid)) continue;
      const [px, py] = worldCmToScreenOffset(poi.x_cm, poi.y_cm, viewport, state.camera);
      const d = Math.hypot(px - sx, py - sy), hitR = Math.max(18, Math.min(22, (poi.icon_size ?? 24) * 0.75));
      const isEmber = /wayshrine|emberspring|attune|shrine/i.test(`${poi.category} ${poi.kind ?? ""} ${poi.icon_name}`);
      if (d <= hitR && (d < bestPoiDist || (isEmber && bestPoi && !/wayshrine|emberspring|attune|shrine/i.test(`${bestPoi.category} ${bestPoi.icon_name}`) && d <= bestPoiDist + 5))) {
        bestPoiDist = d; bestPoi = poi;
      }
    }
  }
  let bestNode: VerraCityNodeMeta | null = null, bestNodeDist = 18;
  if (state.layers.pois && state.layers.cityNodes) {
    for (const node of state.cityNodes) {
      const { cx_cm, cy_cm } = getCityNodeCenterCm(node);
      const [nx, ny] = worldCmToScreenOffset(cx_cm, cy_cm, viewport, state.camera);
      const d = Math.hypot(nx - sx, ny - sy);
      if (d <= bestNodeDist) { bestNodeDist = d; bestNode = node; }
    }
  }
  if (bestEnt && ((bestEnt.npcSubkind ?? bestEnt.npc_subkind) === "emberspring" || bestEnt.kind === "poi" || bestEnt.kind === "player") && bestEntDist <= bestPoiDist && bestEntDist <= bestNodeDist) return { entity: bestEnt };
  if (bestPoi && (!bestNode || bestPoiDist <= bestNodeDist + (bestPoi.category === "city_node" ? -2 : 4)) && (!bestEnt || bestPoiDist <= bestEntDist + 6)) return { poi: bestPoi };
  if (bestNode && (!bestEnt || bestNodeDist <= bestEntDist + 4)) return { node: bestNode };
  return bestEnt ? { entity: bestEnt } : bestPoi ? { poi: bestPoi } : bestNode ? { node: bestNode } : null;
}

export function filterMapSearchItems(
  query: string,
  cityNodes: readonly VerraCityNodeMeta[],
  entities: ReadonlyMap<string, WorldEntity>,
  remoteHits: readonly MapSearchResultItem[] = [],
  pois: readonly PoiOverlay[] = [],
): readonly MapSearchResultItem[] {
  const q = query.trim().toLowerCase();
  if (q.length === 0) return [];
  const results: MapSearchResultItem[] = [];
  for (const ent of entities.values()) {
    if (ent.name.toLowerCase().includes(q) || ent.guid.toLowerCase().includes(q) || ent.kind.toLowerCase().includes(q)) {
      const category = ent.kind === "player" ? "player" : ent.kind === "monster" ? "monster" : ent.kind === "resource_node" ? "resource_node" : "npc";
      results.push({ id: `ent:${ent.guid}`, label: `${ent.name} (${ent.kind} Lv ${ent.level})`, category, xCm: ent.xCm, yCm: ent.yCm, zCm: ent.zCm, playerName: ent.kind === "player" ? ent.name : undefined, entityGuid: ent.guid });
    }
  }
  for (const node of cityNodes) {
    if (node.display_name.toLowerCase().includes(q) || node.name.toLowerCase().includes(q) || node.guid.toLowerCase().includes(q)) {
      const { cx_cm, cy_cm } = getCityNodeCenterCm(node);
      results.push({ id: `node:${node.name}`, label: `${node.display_name} (City Node Lv ${node.level})`, category: "node", xCm: cx_cm, yCm: cy_cm, zCm: node.oz ?? 0, nodeName: node.name });
    }
  }
  for (const poi of pois) {
    if (poi.display_name.toLowerCase().includes(q) || poi.name.toLowerCase().includes(q) || poi.category.toLowerCase().includes(q)) {
      results.push({ id: `poi:${poi.guid}`, label: `${poi.display_name || poi.name} (${poi.category || "POI"})`, category: "poi", xCm: poi.x_cm, yCm: poi.y_cm, zCm: poi.z_cm, poiGuid: poi.guid });
    }
  }
  for (const hit of remoteHits) { if (!results.some((r) => r.id === hit.id)) results.push(hit); }
  return results.slice(0, 20);
}

export function resolvePoiIconName(rawIconOrKind: string, fallbackKind = ""): string {
  const cleaned = rawIconOrKind.trim().replace(/\.png$/i, "");
  if (/wayshrine|emberspring|attune|shrine/i.test(`${cleaned} ${fallbackKind}`)) return POI_FALLBACK_ICON_BY_KIND.emberspring;
  if (cleaned.startsWith("TUI_Icon_")) return cleaned;
  const byKind = POI_FALLBACK_ICON_BY_KIND[cleaned.toLowerCase()] ?? POI_FALLBACK_ICON_BY_KIND[fallbackKind.trim().toLowerCase()];
  return byKind ?? (cleaned || "TUI_Icon_Map_MinorPOI_Discovered");
}

export function parsePoisFromPayload(raw: unknown): readonly PoiOverlay[] {
  const obj = (raw && typeof raw === "object" ? raw : {}) as Record<string, unknown>;
  const list = Array.isArray(raw) ? (raw as readonly Record<string, unknown>[]) : Array.isArray(obj.pois) ? (obj.pois as readonly Record<string, unknown>[]) : Array.isArray(obj.places) ? (obj.places as readonly Record<string, unknown>[]) : normalizePaginatedEnvelope<Record<string, unknown>>(obj, ["pois", "places", "items"], 2000, 0).items;
  const out: PoiOverlay[] = [], seen = new Set<string>();
  for (const item of list) {
    const rawGuid = String(item.guid ?? item.id ?? "").trim(), name = String(item.name ?? item.display_name ?? rawGuid).trim();
    if (!name && !rawGuid) continue;
    const x_cm = Number(item.x_cm ?? item.x ?? item.ox ?? 0), y_cm = Number(item.y_cm ?? item.y ?? item.oy ?? 0), z_cm = Number(item.z_cm ?? item.z ?? item.oz ?? 0);
    const category = String(item.category ?? item.kind ?? "poi").trim() || "poi", kind = String(item.kind ?? category).trim() || category;
    const baseGuid = rawGuid && rawGuid !== "0" ? rawGuid : `${category}:${name || `${Math.round(x_cm)}_${Math.round(y_cm)}`}`;
    const guid = seen.has(baseGuid) ? `${baseGuid}:${name}:${Math.round(x_cm)}:${Math.round(y_cm)}` : baseGuid;
    seen.add(guid);
    const iconName = resolvePoiIconName(String(item.icon_name ?? item.icon ?? category), kind), rad = Number(item.radius ?? item.radius_cm ?? 0);
    out.push({
      guid, name: name || guid, display_name: String(item.display_name ?? (name || guid)), category, kind, x_cm, y_cm, z_cm,
      node_name: typeof item.node_name === "string" && item.node_name.trim() ? item.node_name.trim() : undefined,
      node_type: typeof item.node_type === "string" && item.node_type.trim() ? item.node_type.trim() : undefined,
      biome: typeof item.biome === "string" && item.biome.trim() ? item.biome.trim() : undefined,
      radius: Number.isFinite(rad) && rad > 0 ? rad : undefined, yaw: Number.isFinite(Number(item.yaw ?? item.yaw_deg)) ? Number(item.yaw ?? item.yaw_deg) : undefined,
      level: Number.isFinite(Number(item.level)) ? Number(item.level) : undefined, min_level: Number.isFinite(Number(item.min_level)) ? Number(item.min_level) : undefined,
      max_level: Number.isFinite(Number(item.max_level)) ? Number(item.max_level) : undefined, levelable: typeof item.levelable === "boolean" ? item.levelable : undefined,
      stage_name: typeof item.stage_name === "string" && item.stage_name.trim() ? item.stage_name.trim() : undefined,
      description: typeof (item.description ?? item.descriptor) === "string" && String(item.description ?? item.descriptor).trim() ? String(item.description ?? item.descriptor).trim() : undefined,
      tags: Array.isArray(item.tags) ? item.tags.map(String) : undefined, icon_name: iconName, icon_url: String(item.icon_url ?? `/api/v1/world/icons/${iconName}.png`),
      tint_color: typeof item.tint_color === "string" ? item.tint_color : undefined, icon_size: typeof item.icon_size === "number" ? item.icon_size : 22,
    });
  }
  return out;
}

export function mergePoiOverlays(existing: readonly PoiOverlay[], incoming: readonly PoiOverlay[]): readonly PoiOverlay[] {
  if (existing.length === 0) return incoming;
  if (incoming.length === 0) return existing;
  const byKey = new Map<string, PoiOverlay>();
  for (const p of existing) byKey.set(p.guid || p.name, p);
  for (const p of incoming) byKey.set(p.guid || p.name, { ...byKey.get(p.guid || p.name), ...p });
  return [...byKey.values()];
}

export const renderVerraMapFrame = drawCanvasScene;
export const renderMapImageOverlays = drawNodePlugOverlays;
export const renderEntityMarkers = drawEntityAndCrosshairMarkers;
