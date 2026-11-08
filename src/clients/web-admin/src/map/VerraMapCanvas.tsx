// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import { EntityInspector, formatPoiCategoryDisplay, poiOverlayToInspectedEntity } from "../components/EntityInspector";
import {
  buildPaginatedQueryUrl,
  buildSpawnNpcCommand,
  buildTeleportToCoordsCommand,
  fetchWithTransientRetry,
  GmCommandPalette,
  type GmRole,
  normalizePaginatedEnvelope,
  sendGmCommand,
} from "../components/GmCommandPalette";
import {
  dispatchAuxiliaryGmAction,
  GmActionModalsDock,
  NodeGovernanceModalCard,
  refreshWorldEntitiesAfterNodeTierChange,
  renderNodeGovernanceModalCard,
  SANDAL_LAUNCHER_TOKENS,
} from "../components/PlayersAndSpawnerTab";
import { StreamStatus } from "../components/StreamStatus";
import { isUnauthenticatedViewOnly, useRetryResetEpoch } from "../state/adminAuthClient";
import {
  buildActiveLayersParam,
  buildWorldStatePollUrl,
  computeLayerEntityCounts,
  computeLiveTimeOfDayHours,
  computeStreamLayerMask,
  computeViewportWorldBoundsCm,
  DEFAULT_CAMERA_VIEWPORT,
  DEFAULT_VERRA_CITY_NODES,
  DEFAULT_VERRA_MAP_META,
  formatCanonicalWorkerName,
  formatTimeOfDayLabel,
  formatTimePhaseLabel,
  formatWorkerTickHzOptionLabel,
  getCityNodeCenterCm,
  getMapPollHzForZoom,
  getMapPollIntervalMsForZoom,
  getVisibleWorkersForViewport,
  inferResourceNodeType,
  type LayerToggles,
  normalizeWorkerTickHz,
  type NpcInstanceAction,
  OVERLAY_LAYER_KEYS,
  parseTimeOfDayInput,
  resolveWorkerForCoordinateOrZone,
  resolveWorkerTickFields,
  shouldFetchPoisCatalog,
  useAdminAuthStore,
  useWorldStreamStore,
  VERRA_MAP_SOURCE_CM,
  VERRA_MAP_SOURCE_PX,
  type VerraMapMeta,
  type VerraUnrealPoint,
  type WorldEntity,
  type WorldStreamState,
  type WorldWorkerInfo,
  WORKER_TICK_HZ_OPTIONS,
  zoomToWorkerArea,
} from "../state/worldStreamStore";
import {
  dispatchAuthoritativeTimeAndCalendarUpdate,
  pollAuthoritativeTimeOfDay,
  useLiveVerraClockAndCalendar,
  VerraCalendarDateModifierSection,
} from "./VerraCalendarPopover";
import {
  buildVerraDeckOverlayLayers,
  DEFAULT_VERRA_PMTILES_URL,
  drawCanvasScene,
  ENTITY_MAP_LEGEND_SWATCHES,
  filterMapSearchItems,
  findNearestEntityOrNodeAtScreen,
  formatOverlayLayerLabel,
  handleCanvasRightClickClear,
  hydrateVerraZoiPolygonsFromTsvApi,
  isEntityVisibleForLayers,
  type MapSearchResultItem,
  MAX_VERRA_MAP_ZOOM,
  mergePoiOverlays,
  MIN_VERRA_MAP_ZOOM,
  parsePoisFromPayload,
  PURE_BLACK_TILE_BG,
  resolveNativeCanvasRenderArea,
  resolveVerraMapViewState,
  screenOffsetToWorldCm,
  VerraTileCache,
} from "./verraTilePyramidRenderer";

export { formatPoiCategoryDisplay, poiOverlayToInspectedEntity };
export * from "../state/worldStreamStore";
export * from "./VerraCalendarPopover";
export * from "./verraTilePyramidRenderer";

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

export const NPC_INSTANCE_ACTIONS: readonly { readonly id: NpcInstanceAction; readonly label: string }[] = Object.freeze([
  { id: "kill_npc", label: "Kill" },
  { id: "heal_npc", label: "Heal" },
  { id: "reset_npc", label: "Reset" },
  { id: "respawn_npc", label: "Respawn" },
  { id: "despawn_npc", label: "Despawn" },
]);

function applyPolledWorldState(stateData: unknown, snap: WorldStreamState): void {
  const added = parseWorldStateResponse(stateData, snap.layers), pois = parsePoisFromPayload(stateData);
  if (pois.length > 0) snap.setPois(mergePoiOverlays(snap.pois, pois));
  if (Array.isArray((stateData as { workers?: unknown }).workers)) snap.setWorkers((stateData as { workers: readonly WorldWorkerInfo[] }).workers);
  const sd = stateData as { time_of_day_hours?: unknown; time_of_day_anchor_hours?: unknown; calendar_day_of_year?: unknown; calendar_year?: unknown };
  const liveH = typeof sd.time_of_day_hours === "number" ? sd.time_of_day_hours : typeof sd.time_of_day_anchor_hours === "number" ? sd.time_of_day_anchor_hours : null;
  const now = Date.now();
  if (liveH !== null && Number.isFinite(liveH)) snap.setTimeOfDay(liveH, now);
  if (typeof sd.calendar_day_of_year === "number" && Number.isFinite(sd.calendar_day_of_year)) {
    snap.setWorldCalendar(sd.calendar_day_of_year, typeof sd.calendar_year === "number" ? sd.calendar_year : 1, now);
  }
  snap.applyWorldDelta({ seq: snap.lastSeq + 1, isSnapshot: true, added });
}

function useWorldMapPolling(storeSnap: WorldStreamState, onMetaLoaded: (meta: VerraMapMeta) => void, canvasRef: React.RefObject<HTMLCanvasElement | null>): void {
  const authSnap = useAdminAuthStore();
  const retryEpoch = useRetryResetEpoch();
  const isUnauthed = Boolean(authSnap.authRequired || authSnap.loginPromptOpen);
  const activeLayersParam = buildActiveLayersParam(storeSnap.layers);
  const pollIntervalMs = getMapPollIntervalMsForZoom(storeSnap.camera.zoom);
  const storeSnapRef = React.useRef(storeSnap);
  storeSnapRef.current = storeSnap;
  const pollRef = React.useRef<(() => void) | null>(null);
  React.useEffect(() => {
    if (isUnauthed) { storeSnapRef.current.resetForUnauthenticatedSession(); return; }
    if (typeof window === "undefined" || typeof fetch !== "function") return;
    let cancelled = false;
    Promise.all([
      fetchWithTransientRetry("/api/v1/world/map-meta").then((r) => (r.ok ? r.json() : null)).catch(() => null),
      fetchWithTransientRetry("/api/v1/world/tiles/metadata.json").then((r) => (r.ok ? r.json() : null)).catch(() => null),
      fetchWithTransientRetry("/api/v1/world/zois").then((r) => (r.ok ? r.json() : null)).catch(() => null),
      fetchWithTransientRetry("/api/v1/catalog/places?limit=2000").then((r) => (r.ok ? r.json() : null)).catch(() => null),
    ]).then(([metaData, tileMeta, zoisData, placesData]) => {
      if (cancelled || isUnauthenticatedViewOnly()) return;
      const zoiItems = Array.isArray((zoisData as { zois?: unknown })?.zois) ? (zoisData as { zois: unknown[] }).zois : Array.isArray(zoisData) ? zoisData : [];
      if (zoiItems.length > 0) hydrateVerraZoiPolygonsFromTsvApi(zoiItems as Parameters<typeof hydrateVerraZoiPolygonsFromTsvApi>[0]);
      const catalogPois = parsePoisFromPayload(placesData);
      if (catalogPois.length > 0) storeSnapRef.current.setPois(mergePoiOverlays(storeSnapRef.current.pois, catalogPois));
      const merged = { ...(tileMeta ?? {}), ...(metaData ?? {}) } as Partial<VerraMapMeta>;
      const nodes = Array.isArray(merged.nodes) && merged.nodes.length > 0 ? merged.nodes : DEFAULT_VERRA_CITY_NODES;
      onMetaLoaded({ ...DEFAULT_VERRA_MAP_META, ...merged, nodes });
      storeSnapRef.current.setCityNodes(nodes);
      storeSnapRef.current.setConnectionState("live");
    });
    return () => { cancelled = true; };
  }, [isUnauthed, authSnap.token, retryEpoch]);

  React.useEffect(() => {
    if (isUnauthed || typeof window === "undefined" || typeof fetch !== "function" || !buildWorldStatePollUrl(storeSnap.layers)) { pollRef.current = null; return; }
    let cancelled = false, inflight = false, rerunAfterInflight = false, tickCount = 0;
    const poll = (): void => {
      if (cancelled || isUnauthenticatedViewOnly()) return;
      if (inflight) { rerunAfterInflight = true; return; }
      const snap = storeSnapRef.current;
      const w = canvasRef.current?.clientWidth || (typeof window !== "undefined" && window.innerWidth > 0 ? window.innerWidth : 1920);
      const h = canvasRef.current?.clientHeight || (typeof window !== "undefined" && window.innerHeight > 0 ? window.innerHeight : 1080);
      const baseUrl = buildWorldStatePollUrl(snap.layers, computeViewportWorldBoundsCm(snap.camera, w, h));
      if (!baseUrl) return;
      tickCount += 1;
      const pollUrl = `${baseUrl}&compact=1${snap.pois.length > 0 && tickCount % 90 !== 1 ? "&include_pois=0" : ""}`;
      inflight = true;
      fetchWithTransientRetry(pollUrl).then((r) => (r.ok ? r.json() : null)).then((stateData) => {
        if (!cancelled && stateData && !isUnauthenticatedViewOnly()) applyPolledWorldState(stateData, snap);
      }).catch(() => {}).finally(() => {
        inflight = false;
        if (rerunAfterInflight && !cancelled) { rerunAfterInflight = false; poll(); }
      });
    };
    pollRef.current = poll;
    pollAuthoritativeTimeOfDay(storeSnapRef);
    poll();
    const timer = window.setInterval(poll, pollIntervalMs);
    return () => { cancelled = true; pollRef.current = null; window.clearInterval(timer); };
  }, [isUnauthed, authSnap.token, retryEpoch, activeLayersParam, pollIntervalMs]);

  React.useEffect(() => { pollRef.current?.(); }, [storeSnap.camera.centerXCm, storeSnap.camera.centerYCm, storeSnap.camera.zoom]);
}

export function parseWorldStateResponse(raw: unknown, layers: LayerToggles): readonly WorldEntity[] {
  const obj = (raw && typeof raw === "object" ? raw : {}) as Record<string, unknown>;
  const added: WorldEntity[] = [], seenPlayers = new Set<string>();
  if (layers.players) {
    for (const p of normalizePaginatedEnvelope<Record<string, unknown>>(obj.players ?? obj, ["players"]).items) {
      if (p.online === false || String(p.state ?? "").toLowerCase() === "offline") continue;
      const guid = String(p.character_id ?? p.guid ?? p.name ?? "player"), name = String(p.name ?? "Player");
      const gKey = guid.trim().toLowerCase(), nKey = name.trim().toLowerCase();
      if ((gKey && seenPlayers.has(gKey)) || (nKey && seenPlayers.has(nKey))) continue;
      if (gKey) seenPlayers.add(gKey);
      if (nKey) seenPlayers.add(nKey);
      added.push({
        guid, name, kind: "player",
        xCm: Number(p.x_cm ?? p.x ?? 0), yCm: Number(p.y_cm ?? p.y ?? 0), zCm: Number(p.z_cm ?? p.z ?? 0),
        yawDeg: Number(p.yaw_deg ?? p.yaw_degrees ?? 0), level: Number(p.level ?? 1), healthPct: 100,
        ...(Boolean(p.mounted || p.is_mounted || p.mount_guid || p.mount_id) ? { mounted: true } : {}),
      } as WorldEntity);
    }
  }
  const rawEntities = (Array.isArray(obj.entities) ? obj.entities : []) as readonly Record<string, unknown>[];
  for (const e of rawEntities) {
    const rawKind = String(e.kind ?? (e.hostile ? "monster" : "npc"));
    const rawName = String(e.name ?? rawKind);
    const explicitSub = typeof e.npc_subkind === "string" && e.npc_subkind.length > 0 ? e.npc_subkind : typeof e.npcSubkind === "string" && e.npcSubkind.length > 0 ? e.npcSubkind : undefined;
    const isEmber = rawKind === "emberspring" || rawKind === "wayshrine" || explicitSub === "emberspring" || /\bemberspring\b|ember\s+spring|revive\s*shrine|attunable.*shrine|\bwayshrine\b/i.test(rawName);
    const rawSubkind = isEmber ? "emberspring" : explicitSub;
    const kind = rawKind === "player" || rawKind === "monster" || rawKind === "resource_node" || rawKind === "mount" || rawKind === "loot_pile" || rawKind === "poi" ? rawKind : "npc";
    const guid = String(e.id ?? e.guid ?? e.name ?? "entity");
    if (kind === "player") {
      if (e.online === false || String(e.state ?? "").toLowerCase() === "offline") continue;
      const gKey = guid.trim().toLowerCase(), nKey = rawName.trim().toLowerCase();
      if ((gKey && seenPlayers.has(gKey)) || (nKey && seenPlayers.has(nKey))) continue;
      if (gKey) seenPlayers.add(gKey);
      if (nKey) seenPlayers.add(nKey);
    }
    const rawResType = typeof e.resourceType === "string" && e.resourceType.length > 0 ? e.resourceType : typeof e.resource_type === "string" && e.resource_type.length > 0 ? e.resource_type : undefined;
    const resType = kind === "resource_node" || rawSubkind === "huntable" || Boolean(rawResType) ? inferResourceNodeType(rawName, rawResType, rawSubkind) : undefined;
    const qual = typeof e.quality === "string" && e.quality.length > 0 ? e.quality : typeof e.rarity === "string" && e.rarity.length > 0 ? e.rarity : kind === "resource_node" ? "Common · Tier 1 (Novice)" : undefined;
    const ent: WorldEntity = {
      guid, name: rawName, kind,
      xCm: Number(e.x_cm ?? e.x ?? 0), yCm: Number(e.y_cm ?? e.y ?? 0), zCm: Number(e.z_cm ?? e.z ?? 0),
      yawDeg: Number(e.yaw_deg ?? e.yaw ?? 0), level: Number(e.level ?? 1), healthPct: Number(e.health_pct ?? e.hp ?? 100),
      npc_subkind: rawSubkind, npcSubkind: rawSubkind, resourceType: resType, resource_type: resType, quality: qual,
      poiCategory: isEmber ? "Ember Spring" : kind === "poi" ? formatPoiCategoryDisplay({ category: String(e.category ?? rawKind), name: rawName }) : undefined,
      zoneId: typeof e.zone === "string" ? e.zone : typeof e.zone_id === "string" ? e.zone_id : undefined,
      ...(kind === "player" && Boolean(e.mounted || e.is_mounted || e.mount_guid || e.mount_id) ? { mounted: true } : {}),
    } as WorldEntity;
    if (kind === "mount") {
      for (const pl of added) {
        if (pl.kind === "player" && Math.hypot(pl.xCm - ent.xCm, pl.yCm - ent.yCm) <= 250) (pl as { mounted?: boolean }).mounted = true;
      }
    }
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
        next.push({
          id: `remote-player:${String(p.name)}`,
          label: `${String(p.name)} (Online Player)`,
          category: "player",
          playerName: String(p.name),
          xCm: Number(p.x_cm ?? p.x ?? 0),
          yCm: Number(p.y_cm ?? p.y ?? 0),
          zCm: Number(p.z_cm ?? p.z ?? 0),
        });
      }
      for (const pl of normalizePaginatedEnvelope<Record<string, unknown>>(plRes, ["places"]).items) {
        next.push({
          id: `remote-place:${String(pl.name ?? pl.guid)}`,
          label: `${String(pl.display_name ?? pl.name)} (Place)`,
          category: "place",
          xCm: Number(pl.x_cm ?? pl.x ?? pl.ox ?? 0),
          yCm: Number(pl.y_cm ?? pl.y ?? pl.oy ?? 0),
          zCm: Number(pl.z_cm ?? pl.z ?? 0),
        });
      }
      setHits(next);
    });
    return () => {
      cancelled = true;
    };
  }, [searchText, layers.players, layers.pois]);
  return hits;
}

export function ServerTimeOfDayControl(props: { readonly storeSnap: WorldStreamState; readonly compact?: boolean }): React.ReactElement {
  const { storeSnap, compact } = props;
  const { liveHours, liveClock, liveCalendar, setLiveAnchor } = useLiveVerraClockAndCalendar(storeSnap);
  const [popoverOpen, setPopoverOpen] = React.useState(false);
  const [draftTime, setDraftTime] = React.useState(() => formatTimeOfDayLabel(storeSnap.timeOfDayHours ?? 12));
  const [draftDayOfYear, setDraftDayOfYear] = React.useState(() => storeSnap.calendarDayOfYear ?? 1);
  const [draftYear, setDraftYear] = React.useState(() => storeSnap.calendarYear ?? 1);
  React.useEffect(() => { setDraftTime(formatTimeOfDayLabel(storeSnap.timeOfDayHours ?? 12)); }, [storeSnap.timeOfDayHours]);
  React.useEffect(() => { setDraftDayOfYear(storeSnap.calendarDayOfYear ?? 1); setDraftYear(storeSnap.calendarYear ?? 1); }, [storeSnap.calendarDayOfYear, storeSnap.calendarYear]);
  const draftHours = parseTimeOfDayInput(draftTime);
  const draftMinutes = Math.max(0, Math.min(1439, Math.round(draftHours * 60)));
  const applyHours = (h: number, d = draftDayOfYear, y = draftYear): void => {
    const clean = (((Number.isFinite(h) ? h : 12) % 24) + 24) % 24;
    setLiveAnchor(clean, d, y);
    setDraftTime(formatTimeOfDayLabel(clean));
    setDraftDayOfYear(d);
    setDraftYear(y);
    setPopoverOpen(false);
    dispatchAuthoritativeTimeAndCalendarUpdate(storeSnap, clean, d, y);
  };
  return (
    <div style={{ position: "relative", zIndex: popoverOpen ? 100 : 10, display: "inline-flex", alignItems: "center", gap: "6px", padding: compact ? "2px 6px" : "3px 8px", borderRadius: "6px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush }}>
      <span aria-hidden="true" style={{ fontSize: "12px", color: liveHours >= 6 && liveHours < 20 ? SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400 : "#93C5FD" }}>{liveHours >= 6 && liveHours < 20 ? "\u2600" : "\u263E"}</span>
      <span data-testid="world-current-time-display" title="Authoritative Verra Server Time (advances at 9x real time across all world workers)" style={{ fontFamily: "ui-monospace, monospace", fontVariantNumeric: "tabular-nums", fontSize: "11px", fontWeight: 700, color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400 }}>
        Time {liveClock}
      </span>
      <span data-testid="verra-calendar-badge" title={`Year ${liveCalendar.calendarYear}, ${liveCalendar.calendarSeason} Day ${liveCalendar.calendarDayOfSeason} of 90 (Day ${liveCalendar.calendarDayOfYear} of 360)`} style={{ fontSize: "10px", fontWeight: 600, color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>
        {compact ? `${liveCalendar.calendarSeason} D${liveCalendar.calendarDayOfSeason}` : liveCalendar.calendarFormatted}
      </span>
      <button type="button" data-testid="world-time-picker-btn" aria-expanded={popoverOpen} title="Open Server Time of Day & Verra Calendar editor" onClick={() => { if (!popoverOpen) { setDraftTime(formatTimeOfDayLabel(liveHours)); setDraftDayOfYear(liveCalendar.calendarDayOfYear); setDraftYear(liveCalendar.calendarYear); } setPopoverOpen((v) => !v); }} style={{ height: compact ? "22px" : "24px", padding: "0 8px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.VerraBorder400}`, backgroundColor: popoverOpen ? SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: popoverOpen ? SANDAL_LAUNCHER_TOKENS.VerraObsidian100 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontFamily: "inherit", fontSize: "11px", fontWeight: 700, cursor: "pointer" }}>
        Set Time
      </button>
      <div id="time-of-day-popover" data-testid="world-time-picker-popover" style={{ display: popoverOpen ? "flex" : "none", position: "absolute", top: "calc(100% + 6px)", right: 0, width: "340px", flexDirection: "column", gap: "8px", padding: "10px 12px", borderRadius: "8px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.VerraBorder400}`, background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush, backdropFilter: "blur(18px)", boxShadow: "0 12px 32px rgba(0,0,0,0.65)", zIndex: 1200 }}>
        <div data-testid="time-of-day-popover" style={{ display: "flex", justifyContent: "space-between", alignItems: "center", fontSize: "11px" }}>
          <strong style={{ color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500, letterSpacing: "0.04em", textTransform: "uppercase" }}>Server Time of Day</strong>
          <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400, fontFamily: "ui-monospace, monospace", fontWeight: 700 }}>{formatTimeOfDayLabel(draftHours)} ({formatTimePhaseLabel(draftHours)})</span>
        </div>
        <div data-testid="time-of-day-range-slider">
          <input type="range" min={0} max={1439} step={1} aria-label="Scrub 24-hour server time" data-testid="world-time-slider" value={draftMinutes} onChange={(e) => setDraftTime(formatTimeOfDayLabel(Number(e.currentTarget.value) / 60))} style={{ width: "100%", height: "8px", borderRadius: "4px", appearance: "none", background: "linear-gradient(90deg, #0d1326 0%, #c87d55 25%, #f6cc7e 50%, #d97736 75%, #0d1326 100%)", cursor: "pointer" }} />
        </div>
        <div style={{ display: "flex", gap: "6px", alignItems: "center" }}>
          <input type="text" aria-label="Server Time of Day (HH:MM)" data-testid="world-time-of-day-select" placeholder="HH:MM" value={draftTime} onChange={(e) => setDraftTime(e.currentTarget.value)} onKeyDown={(e) => { if (e.key === "Enter") applyHours(parseTimeOfDayInput(draftTime)); }} style={{ flex: 1, height: "28px", padding: "0 8px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontFamily: "ui-monospace, monospace", fontSize: "12px" }} />
          <button type="button" data-testid="world-time-apply-btn" onClick={() => applyHours(parseTimeOfDayInput(draftTime))} style={{ height: "28px", padding: "0 10px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.VerraBorder400}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500, color: SANDAL_LAUNCHER_TOKENS.VerraObsidian100, fontFamily: "inherit", fontSize: "11px", fontWeight: 700, cursor: "pointer" }}>Apply Time</button>
        </div>
        <div style={{ display: "grid", gridTemplateColumns: "repeat(4, 1fr)", gap: "4px" }}>
          {[{ h: 6, label: "06:00 Dawn" }, { h: 12, label: "12:00 Noon" }, { h: 18, label: "18:00 Dusk" }, { h: 0, label: "00:00 Night" }].map((p) => (
            <button key={p.h} type="button" onClick={() => applyHours(p.h)} style={{ height: "24px", padding: "0 4px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontFamily: "inherit", fontSize: "10px", cursor: "pointer" }}>{p.label}</button>
          ))}
        </div>
        <VerraCalendarDateModifierSection draftDayOfYear={draftDayOfYear} draftYear={draftYear} onChangeDate={(d, y) => { setDraftDayOfYear(d); setDraftYear(y); }} onApplyPresetDate={(d, y) => applyHours(draftHours, d, y)} />
      </div>
    </div>
  );
}

function renderMapHeader(
  searchText: string,
  searchResults: readonly MapSearchResultItem[],
  storeSnap: WorldStreamState,
  onSearchChange: (next: string) => void,
  onSelectHit: (hit: MapSearchResultItem) => void,
): React.ReactElement {
  const counts = computeLayerEntityCounts(storeSnap);
  return (
    <header style={{ position: "relative", zIndex: 60, display: "flex", flexWrap: "wrap", alignItems: "center", justifyContent: "space-between", gap: "8px", padding: "8px 14px", background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush, backdropFilter: "blur(16px)", borderBottom: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, fontFamily: '"Segoe UI", system-ui, -apple-system, sans-serif' }}>
      <StreamStatus />
      <div style={{ position: "relative", minWidth: "240px" }}>
        <input type="search" aria-label="Search online players on map" data-testid="verra-map-search-input" value={searchText} onChange={(e) => onSearchChange(e.currentTarget.value)} placeholder="Search online players on map..." style={{ width: "100%", minHeight: "32px", padding: "5px 10px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontFamily: "inherit", fontSize: "12px" }} />
        {searchResults.length > 0 ? (
          <ul role="listbox" aria-label="Player search results" data-testid="verra-map-search-results" style={{ position: "absolute", top: "36px", left: 0, right: 0, maxHeight: "240px", overflowY: "auto", zIndex: 40, margin: 0, padding: "4px", listStyle: "none", borderRadius: "6px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.VerraBorder400}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.VerraObsidian100 }}>
            {searchResults.map((hit) => (
              <li key={hit.id}>
                <button type="button" onClick={() => onSelectHit(hit)} style={{ width: "100%", minHeight: "32px", padding: "4px 8px", textAlign: "left", border: "none", background: "transparent", color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500, fontFamily: "inherit", fontSize: "12px", cursor: "pointer" }}>{hit.label}</button>
              </li>
            ))}
          </ul>
        ) : null}
      </div>
      <GmActionModalsDock />
      <div role="toolbar" aria-label="Map layer visibility toggles" style={{ display: "flex", gap: "5px", flexWrap: "wrap", alignItems: "center" }}>
        {OVERLAY_LAYER_KEYS.map((layerKey) => (
          <button key={layerKey} type="button" data-testid={`layer-toggle-${layerKey}`} aria-pressed={storeSnap.layers[layerKey]} onClick={() => storeSnap.toggleLayer(layerKey)} style={{ minHeight: "32px", padding: "4px 10px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: storeSnap.layers[layerKey] ? SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: storeSnap.layers[layerKey] ? SANDAL_LAUNCHER_TOKENS.VerraObsidian100 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontFamily: "inherit", fontSize: "11px", fontWeight: 700, cursor: "pointer" }}>
            {formatOverlayLayerLabel(layerKey, counts)}
          </button>
        ))}
        <button type="button" data-testid="layer-toggle-zoiBorders" aria-pressed={storeSnap.layers.zoiBorders} onClick={() => storeSnap.toggleLayer("zoiBorders")} style={{ minHeight: "32px", padding: "4px 10px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: storeSnap.layers.zoiBorders ? SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: storeSnap.layers.zoiBorders ? SANDAL_LAUNCHER_TOKENS.VerraObsidian100 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontFamily: "inherit", fontSize: "11px", fontWeight: 700, cursor: "pointer" }}>ZOI Borders</button>
        <ServerTimeOfDayControl storeSnap={storeSnap} />
      </div>
    </header>
  );
}

function renderBottomLeftMapEntityLegend(): React.ReactElement {
  return (
    <div data-testid="map-entity-legend" aria-label="Map Entity Color Legend" style={{ position: "absolute", left: "12px", bottom: "12px", zIndex: 18, display: "flex", flexDirection: "column", alignItems: "flex-start", gap: "5px", padding: "8px 10px", borderRadius: "6px", border: "1px solid rgba(140, 108, 66, 0.47)", backgroundColor: "rgba(11, 14, 23, 0.86)", backdropFilter: "blur(12px)", fontSize: "11px", color: "#B8C4D4" }}>
      {ENTITY_MAP_LEGEND_SWATCHES.map((sw) => (
        <span key={sw.id} data-testid={`legend-swatch-${sw.id}`} style={{ display: "inline-flex", alignItems: "center", gap: "6px", whiteSpace: "nowrap" }}>
          <span aria-hidden="true" style={{ width: "8px", height: "8px", borderRadius: "50%", backgroundColor: sw.color, display: "inline-block" }} />
          <span>{sw.label}</span>
        </span>
      ))}
    </div>
  );
}

function renderZoomAndNodeControls(
  storeSnap: WorldStreamState,
  cityFilterText: string,
  onCityFilterChange: (q: string) => void,
): React.ReactElement {
  const q = cityFilterText.trim().toLowerCase();
  const filteredNodes = q ? storeSnap.cityNodes.filter((n) => `${n.display_name} ${n.name}`.toLowerCase().includes(q)) : storeSnap.cityNodes;
  const jumpToCityNode = (name: string | null): void => {
    storeSnap.selectCityNode(name);
    const found = storeSnap.cityNodes.find((n) => n.name === name);
    if (found) { const { cx_cm, cy_cm } = getCityNodeCenterCm(found); storeSnap.setCameraViewport({ centerXCm: cx_cm, centerYCm: cy_cm, zoom: 5.5 }); }
  };
  return (
    <div style={{ position: "absolute", left: "12px", top: "12px", display: "flex", flexDirection: "column", gap: "6px", zIndex: 18, width: "220px" }}>
      <div style={{ display: "flex", gap: "4px", alignItems: "center", flexWrap: "wrap" }}>
        <button type="button" aria-label="Zoom in" data-testid="map-zoom-in" onClick={() => storeSnap.setCameraViewport({ zoom: Math.min(MAX_VERRA_MAP_ZOOM, storeSnap.camera.zoom + 1) })} style={{ width: "32px", height: "32px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, cursor: "pointer" }}>+</button>
        <button type="button" aria-label="Zoom out" data-testid="map-zoom-out" onClick={() => storeSnap.setCameraViewport({ zoom: Math.max(MIN_VERRA_MAP_ZOOM, storeSnap.camera.zoom - 1) })} style={{ width: "32px", height: "32px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, cursor: "pointer" }}>-</button>
        <button type="button" aria-label="Reset zoom" data-testid="map-zoom-reset" onClick={() => storeSnap.setCameraViewport(DEFAULT_CAMERA_VIEWPORT)} style={{ height: "32px", padding: "0 8px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "11px", cursor: "pointer" }}>Reset</button>
        <div data-testid="map-zoom-hz-badge" style={{ padding: "4px 8px", borderRadius: "6px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush, color: SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500, fontSize: "11px", fontWeight: 600 }}>
          Zoom {storeSnap.camera.zoom.toFixed(1)}
        </div>
      </div>
      <div data-testid="city-quick-jump-combobox" style={{ display: "flex", flexDirection: "column", gap: "4px", padding: "6px", borderRadius: "6px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush }}>
        <input type="search" aria-label="Filter city nodes" data-testid="city-quick-jump-search" placeholder="Search city / node..." value={cityFilterText} onChange={(e) => onCityFilterChange(e.currentTarget.value)} onKeyDown={(e) => { if (e.key === "Enter" && filteredNodes.length > 0) jumpToCityNode(filteredNodes[0].name); }} style={{ minHeight: "28px", padding: "4px 8px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "11px" }} />
        <select aria-label="Select City Node to inspect or change stage" data-testid="city-node-quick-select" data-city-quick-jump-select="true" value={storeSnap.selectedCityNodeName ?? ""} onChange={(e) => jumpToCityNode(e.currentTarget.value || null)} style={{ minHeight: "32px", padding: "4px 6px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "11px" }}>
          <option value="">26 City Nodes...</option>
          {filteredNodes.map((n) => (<option key={n.name} value={n.name}>{n.display_name} (Lv {n.level})</option>))}
        </select>
      </div>
    </div>
  );
}

function renderBottomRightToastAndWorkersDock(storeSnap: WorldStreamState, canvasRef: React.RefObject<HTMLCanvasElement | null>): React.ReactElement {
  const viewportBounds = computeViewportWorldBoundsCm(storeSnap.camera, canvasRef.current?.clientWidth || undefined, canvasRef.current?.clientHeight || undefined);
  const visibleWorkers = getVisibleWorkersForViewport(storeSnap.workers, viewportBounds);
  return (
    <div style={{ position: "absolute", bottom: "12px", right: "12px", zIndex: 18, display: "flex", flexDirection: "column", alignItems: "flex-end", gap: "8px", width: "450px", minWidth: "450px", maxWidth: "450px" }}>
      <div data-testid="gm-action-toast-stack" role="status" aria-live="polite" style={{ display: "flex", flexDirection: "column", gap: "6px", width: "100%" }}>
        {storeSnap.toasts.map((toast) => (
          <div key={toast.id} data-testid="gm-action-toast-item" data-toast-tone={toast.tone} style={{ display: "flex", alignItems: "center", justifyContent: "space-between", gap: "8px", padding: "8px 10px", borderRadius: "6px", border: `1px solid ${toast.tone === "error" ? SANDAL_LAUNCHER_TOKENS.VerraRuinCrimson500 : SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500}`, background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush, backdropFilter: "blur(16px)", color: toast.tone === "error" ? SANDAL_LAUNCHER_TOKENS.VerraRuinCrimson500 : SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500, fontSize: "11px", fontWeight: 600 }}>
            <span style={{ flex: 1, wordBreak: "break-word" }}>{toast.text}</span>
            <button type="button" aria-label="Dismiss toast" onClick={() => storeSnap.dismissToast(toast.id)} style={{ border: "none", background: "transparent", color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, cursor: "pointer", fontSize: "11px", padding: "2px 4px" }}>x</button>
          </div>
        ))}
      </div>
      <div data-testid="visible-server-workers-panel" style={{ width: "450px", minWidth: "450px", maxWidth: "450px", height: "208px", minHeight: "208px", maxHeight: "208px", boxSizing: "border-box", display: "flex", flexDirection: "column", borderRadius: "6px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush, backdropFilter: "blur(16px)", color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "11px", overflow: "hidden" }}>
        <div style={{ padding: "6px 10px", borderBottom: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500, fontWeight: 700, display: "flex", justifyContent: "space-between", alignItems: "center" }}>
          <span>Visible Server Workers ({visibleWorkers.length})</span>
        </div>
        <div data-testid="visible-server-workers-scroll" style={{ height: "176px", minHeight: "176px", maxHeight: "176px", boxSizing: "border-box", overflowY: "auto", scrollbarGutter: "stable", display: "flex", flexDirection: "column", gap: "4px", padding: "6px" }}>
          {visibleWorkers.map((w) => {
            const resolved = resolveWorkerTickFields(w), targetHz = resolved.target_tick_hz ?? -1, currentHz = Math.round(resolved.current_tick_hz ?? resolved.tick_hz ?? 45);
            return (
              <div key={w.worker_id} role="button" tabIndex={0} data-testid={`visible-worker-row-${w.worker_id}`} onClick={() => zoomToWorkerArea(w, storeSnap)} onKeyDown={(e) => { if (e.key === "Enter" || e.key === " ") zoomToWorkerArea(w, storeSnap); }} style={{ width: "100%", height: "30px", minHeight: "30px", maxHeight: "30px", flexShrink: 0, overflow: "hidden", whiteSpace: "nowrap", padding: "4px 6px", boxSizing: "border-box", textAlign: "left", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400, fontSize: "11px", fontWeight: 600, cursor: "pointer", display: "flex", justifyContent: "space-between", alignItems: "center", gap: "6px" }}>
                <span>{formatCanonicalWorkerName(w.scope_kind, w.scope_key)}</span>
                <span style={{ display: "flex", alignItems: "center", gap: "4px", flexShrink: 0, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontWeight: 400, fontSize: "10px" }}>
                  <span>{w.region_label}</span>
                  <select aria-label={`Tick rate for ${w.worker_id}`} data-testid={`worker-tick-rate-select-${w.worker_id}`} value={targetHz} onClick={(e) => e.stopPropagation()} onChange={(e) => { e.stopPropagation(); void storeSnap.executeSetWorkerTickRateAction(w.worker_id, Number(e.currentTarget.value)); }} style={{ height: "20px", padding: "0 4px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush, color: SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500, fontSize: "10px", fontWeight: 700, cursor: "pointer" }}>
                    {WORKER_TICK_HZ_OPTIONS.map((hz) => (<option key={hz} value={hz}>{formatWorkerTickHzOptionLabel(hz, true)}</option>))}
                  </select>
                  <span aria-readonly="true" title="Read-only live actual worker tick rate (Hz)" data-testid={`visible-worker-current-hz-${w.worker_id}`} style={{ height: "20px", padding: "0 6px", display: "inline-flex", alignItems: "center", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: "rgba(6, 8, 14, 0.72)", color: SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500, fontVariantNumeric: "tabular-nums", fontWeight: 700, cursor: "default", userSelect: "none" }}>{currentHz} Hz</span>
                </span>
              </div>
            );
          })}
        </div>
      </div>
    </div>
  );
}

function handleCanvasLeftClick(
  e: React.MouseEvent<HTMLCanvasElement>,
  storeSnap: WorldStreamState,
  onCoordinatePick?: (point: VerraUnrealPoint) => void,
  onEntitySelect?: (entity: WorldEntity | null) => void,
): void {
  const rect = e.currentTarget.getBoundingClientRect();
  const sx = e.clientX - rect.left, sy = e.clientY - rect.top;
  const viewport = { width: Math.max(320, Math.round(rect.width || e.currentTarget.width || 960)), height: Math.max(240, Math.round(rect.height || e.currentTarget.height || 540)) };
  const hit = findNearestEntityOrNodeAtScreen(sx, sy, viewport, storeSnap);
  if (hit?.entity) {
    storeSnap.selectEntity(hit.entity.guid);
    onEntitySelect?.(hit.entity);
    return;
  }
  if (hit?.node) {
    storeSnap.selectCityNode(hit.node.name);
    return;
  }
  if (hit?.poi) {
    storeSnap.selectPoi(hit.poi.guid);
    onEntitySelect?.(poiOverlayToInspectedEntity(hit.poi, storeSnap.workers));
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
  onEntitySelect?: (entity: WorldEntity | null) => void,
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
      onKeyDown={(e) => {
        if (e.key === "Escape") handleCanvasRightClickClear(storeSnap);
      }}
      onMouseDown={(e) => {
        if (e.button === 0) dragRef.current = { active: true, x: e.clientX, y: e.clientY, moved: false };
      }}
      onMouseMove={(e) => {
        if (!dragRef.current.active) {
          const rect = e.currentTarget.getBoundingClientRect();
          const vp = { width: Math.max(320, Math.round(rect.width || e.currentTarget.width || 960)), height: Math.max(240, Math.round(rect.height || e.currentTarget.height || 540)) };
          e.currentTarget.style.cursor = findNearestEntityOrNodeAtScreen(e.clientX - rect.left, e.clientY - rect.top, vp, storeSnap) ? "pointer" : "grab";
          return;
        }
        const dx = e.clientX - dragRef.current.x, dy = e.clientY - dragRef.current.y;
        if (!dragRef.current.moved && Math.hypot(dx, dy) <= 3) return;
        dragRef.current.moved = true;
        dragRef.current.x = e.clientX;
        dragRef.current.y = e.clientY;
        const cmPerPx = (VERRA_MAP_SOURCE_CM / VERRA_MAP_SOURCE_PX) / Math.pow(2, storeSnap.camera.zoom - 5);
        storeSnap.setCameraViewport({ centerXCm: storeSnap.camera.centerXCm - dx * cmPerPx, centerYCm: storeSnap.camera.centerYCm - dy * cmPerPx });
      }}
      onMouseUp={(e) => {
        const wasMoved = dragRef.current.moved;
        dragRef.current.active = false;
        if (e.button === 0 && !wasMoved) handleCanvasLeftClick(e, storeSnap, onCoordinatePick, onEntitySelect);
      }}
      onContextMenu={(e) => {
        e.preventDefault();
        e.stopPropagation();
        handleCanvasRightClickClear(storeSnap);
      }}
      style={{ position: "absolute", inset: 0, display: "block", width: "100%", height: "100%", boxSizing: "border-box", cursor: "grab", backgroundColor: PURE_BLACK_TILE_BG }}
    ></canvas>
  );
}

function formatNpcActionProse(actId: string, entity: WorldEntity): string {
  const pos = `(${Math.round(entity.xCm)}, ${Math.round(entity.yCm)}, ${Math.round(entity.zCm)})`;
  if (actId === "kill_npc") return `Killed ${entity.name} (${entity.guid}) at ${pos}`;
  if (actId === "heal_npc") return `Healed ${entity.name} (${entity.guid}) to 100% HP`;
  if (actId === "reset_npc") return `Reset ${entity.name} (${entity.guid})`;
  if (actId === "respawn_npc") return `Respawned ${entity.name} (${entity.guid}) at ${pos}`;
  return `Despawned ${entity.name} (${entity.guid})`;
}

function SelectedEntityQuickActions(props: { readonly entity: WorldEntity; readonly storeSnap: WorldStreamState }): React.ReactElement {
  const { entity, storeSnap } = props;
  const nudgeMeters = storeSnap.nudgeMeters ?? "8";
  const setNudgeMeters = (val: string): void => storeSnap.patchSessionUi({ nudgeMeters: val });
  const worker = resolveWorkerForCoordinateOrZone(entity.xCm, entity.yCm, entity.zoneId, storeSnap.workers, entity.name);
  const isNpcOrMonster = entity.kind === "npc" || entity.kind === "monster";
  const isPlayer = entity.kind === "player";
  const triggerAxisNudge = (axis: "X" | "Y" | "Z"): void => {
    const meters = Number(nudgeMeters) || 0, deltaCm = Math.round(meters * 100);
    const nextX = Math.round(entity.xCm + (axis === "X" ? deltaCm : 0)), nextY = Math.round(entity.yCm + (axis === "Y" ? deltaCm : 0)), nextZ = Math.round(entity.zCm + (axis === "Z" ? deltaCm : 0));
    void dispatchAuxiliaryGmAction({ action: "nudge_npc", entity_guid: entity.guid, x_cm: nextX, y_cm: nextY, z_cm: nextZ, zone: worker.scope_key, worker_id: worker.worker_id }).then((r) => {
      storeSnap.setScopedReceipt("entityAction", { ok: r.ok, message: `Nudged ${entity.name} (${entity.guid}) ${meters >= 0 ? "+" : ""}${meters}m ${axis} to (${nextX}, ${nextY}, ${nextZ})` });
      storeSnap.applyWorldDelta({ seq: storeSnap.lastSeq + 1, updated: [{ ...entity, xCm: nextX, yCm: nextY, zCm: nextZ }] });
    });
  };
  const nudgeBtnStyle = { minHeight: "28px", padding: "3px 8px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400, fontSize: "11px", cursor: "pointer" } as const;
  return (
    <div data-testid="selected-entity-quick-actions" style={{ position: "relative", width: "100%", boxSizing: "border-box", padding: "10px 12px", borderRadius: "6px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush, backdropFilter: "blur(16px)", color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "12px", zIndex: 20 }}>
      {entity.kind === "resource_node" || Boolean(entity.resourceType ?? entity.resource_type) || (entity.npcSubkind ?? entity.npc_subkind) === "huntable" ? (
        <div style={{ display: "flex", gap: "6px", flexWrap: "wrap", marginBottom: "6px", fontSize: "11px" }}>
          <span data-testid="selected-resource-type" style={{ padding: "2px 7px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush, color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400, fontWeight: 700 }}>Type: {entity.resourceType ?? entity.resource_type ?? inferResourceNodeType(entity.name, undefined, entity.npcSubkind ?? entity.npc_subkind)}</span>
          <span data-testid="selected-resource-quality" style={{ padding: "2px 7px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush, color: SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500, fontWeight: 700 }}>Quality: {entity.quality || "Common · Tier 1 (Novice)"}</span>
        </div>
      ) : null}
      {isNpcOrMonster ? (
        <>
          <div style={{ display: "flex", gap: "4px", flexWrap: "wrap", marginBottom: "6px" }}>
            {NPC_INSTANCE_ACTIONS.map((act) => (
              <button key={act.id} type="button" data-testid={`npc-instance-${act.id}`} onClick={() => void dispatchAuxiliaryGmAction({ action: act.id, entity_guid: entity.guid, zone: worker.scope_key, worker_id: worker.worker_id }).then((r) => { const prose = formatNpcActionProse(act.id, entity); storeSnap.setScopedReceipt("entityAction", { ok: r.ok, message: r.ok ? prose : `${prose}: ${r.message}` }); if (r.ok && (act.id === "kill_npc" || act.id === "despawn_npc")) { storeSnap.applyWorldDelta({ seq: storeSnap.lastSeq + 1, removed: [entity.guid] }); storeSnap.selectEntity(null); void refreshWorldEntitiesAfterNodeTierChange(storeSnap); } })} style={{ minHeight: "30px", padding: "4px 8px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "11px", cursor: "pointer" }}>{act.label}</button>
            ))}
          </div>
          <div style={{ display: "flex", gap: "4px", flexWrap: "wrap", alignItems: "center", marginBottom: "6px" }}>
            <span style={{ fontSize: "11px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>Nudge (m)</span>
            <input type="number" step="any" aria-label="Nudge distance (m)" data-testid="npc-nudge-amount-input" value={nudgeMeters} onChange={(e) => setNudgeMeters(e.currentTarget.value)} style={{ width: "54px", minHeight: "28px", padding: "2px 6px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "11px" }} />
            <button type="button" data-testid="npc-instance-nudge" onClick={() => triggerAxisNudge("X")} style={nudgeBtnStyle}>Nudge X</button>
            <button type="button" data-testid="npc-instance-nudge-y" onClick={() => triggerAxisNudge("Y")} style={nudgeBtnStyle}>Nudge Y</button>
            <button type="button" data-testid="npc-instance-nudge-z" onClick={() => triggerAxisNudge("Z")} style={nudgeBtnStyle}>Nudge Z</button>
          </div>
        </>
      ) : null}
      <div style={{ display: "flex", gap: "6px", flexWrap: "wrap" }}>
        {isPlayer ? (
          <>
            <button type="button" data-testid="inspector-give-item-btn" onClick={() => { storeSnap.setSelectedPlayer(entity.name); storeSnap.setActiveModal("give-item"); }} style={{ minHeight: "32px", padding: "4px 8px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.VerraBorder400}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500, color: SANDAL_LAUNCHER_TOKENS.VerraObsidian100, fontWeight: 700, fontSize: "11px", cursor: "pointer" }}>Give Item...</button>
            <button type="button" data-testid="inspector-give-gold-btn" onClick={() => { storeSnap.setSelectedPlayer(entity.name); storeSnap.setActiveModal("give-currency"); }} style={{ minHeight: "32px", padding: "4px 8px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.VerraBorder400}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500, color: SANDAL_LAUNCHER_TOKENS.VerraObsidian100, fontWeight: 700, fontSize: "11px", cursor: "pointer" }}>Give Gold...</button>
            <button type="button" data-testid="inspector-teleport-here-btn" onClick={() => { storeSnap.setSelectedPlayer(entity.name); storeSnap.setPickedCoordinate({ xCm: Math.round(entity.xCm), yCm: Math.round(entity.yCm), zCm: Math.round(entity.zCm) }); storeSnap.setActiveModal("teleport"); }} style={{ minHeight: "32px", padding: "4px 8px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontWeight: 700, fontSize: "11px", cursor: "pointer" }}>Teleport Player...</button>
          </>
        ) : (
          <button type="button" data-testid="inspector-teleport-here-btn" onClick={() => { storeSnap.setPickedCoordinate({ xCm: Math.round(entity.xCm), yCm: Math.round(entity.yCm), zCm: Math.round(entity.zCm) }); storeSnap.setTeleportMode("coords"); storeSnap.setActiveModal("teleport"); }} style={{ minHeight: "32px", padding: "4px 8px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.VerraBorder400}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500, color: SANDAL_LAUNCHER_TOKENS.VerraObsidian100, fontWeight: 700, fontSize: "11px", cursor: "pointer" }}>Teleport Player Here...</button>
        )}
        <button type="button" data-testid="inspector-spawn-next-btn" onClick={() => { storeSnap.setPickedCoordinate({ xCm: Math.round(entity.xCm + 150), yCm: Math.round(entity.yCm + 150), zCm: Math.round(entity.zCm) }); storeSnap.setSpawnTargetMode("coords"); storeSnap.setActiveModal("spawn-npc"); }} style={{ minHeight: "32px", padding: "4px 8px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "11px", cursor: "pointer" }}>Spawn NPC / Monster Here...</button>
      </div>
    </div>
  );
}

function renderPickedCoordinateCard(pin: VerraUnrealPoint, storeSnap: WorldStreamState): React.ReactElement {
  const xCm = Math.round(Number(pin.xCm ?? 0)), yCm = Math.round(Number(pin.yCm ?? 0)), zCm = Math.round(Number(pin.zCm ?? 0));
  const worker = resolveWorkerForCoordinateOrZone(xCm, yCm, undefined, storeSnap.workers);
  return (
    <div data-testid="picked-coordinate-readout" style={{ padding: "12px", borderRadius: "8px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush, backdropFilter: "blur(16px)", color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "12px", display: "flex", flexDirection: "column", gap: "8px" }}>
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center" }}>
        <strong style={{ color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500, textTransform: "uppercase", letterSpacing: "0.06em" }}>Selected Position</strong>
        <button type="button" data-testid="pin-close-btn" onClick={() => handleCanvasRightClickClear(storeSnap)} style={{ minHeight: "26px", padding: "2px 8px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, fontSize: "11px", cursor: "pointer" }}>Close</button>
      </div>
      <div>Coordinates: <strong>({xCm}, {yCm}, {zCm}) cm</strong></div>
      <div style={{ color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400, fontSize: "11px" }}>Worker: {worker.canonical_name ?? worker.worker_id} ({worker.region_label})</div>
      <div style={{ display: "flex", gap: "6px", flexWrap: "wrap" }}>
        <button type="button" data-testid="pin-teleport-here-btn" onClick={() => { storeSnap.setPickedCoordinate({ xCm, yCm, zCm }); storeSnap.setTeleportMode("coords"); storeSnap.setActiveModal("teleport"); }} style={{ minHeight: "32px", padding: "4px 8px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.VerraBorder400}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500, color: SANDAL_LAUNCHER_TOKENS.VerraObsidian100, fontWeight: 700, fontSize: "11px", cursor: "pointer" }}>Teleport Player Here...</button>
        <button type="button" data-testid="pin-spawn-here-btn" onClick={() => { storeSnap.setPickedCoordinate({ xCm, yCm, zCm }); storeSnap.setSpawnTargetMode("coords"); storeSnap.setActiveModal("spawn-npc"); }} style={{ minHeight: "32px", padding: "4px 8px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "11px", cursor: "pointer" }}>Spawn NPC / Monster Here...</button>
      </div>
    </div>
  );
}

function useCanvasNativeWheelAndDraw(
  canvasRef: React.RefObject<HTMLCanvasElement>,
  mapMeta: VerraMapMeta,
  storeSnap: WorldStreamState,
  tileCacheRef: React.MutableRefObject<VerraTileCache | null>,
  onResize: () => void,
): void {
  const storeSnapRef = React.useRef(storeSnap);
  storeSnapRef.current = storeSnap;

  React.useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas || typeof canvas.addEventListener !== "function") return;
    const handleNativeWheel = (ev: WheelEvent): void => {
      ev.preventDefault();
      ev.stopPropagation();
      const snap = storeSnapRef.current;
      const nextZoom = Math.max(MIN_VERRA_MAP_ZOOM, Math.min(MAX_VERRA_MAP_ZOOM, snap.camera.zoom + (ev.deltaY < 0 ? 0.5 : -0.5)));
      snap.setCameraViewport({ zoom: nextZoom });
    };
    canvas.addEventListener("wheel", handleNativeWheel, { passive: false });
    const ro = typeof ResizeObserver === "function" ? new ResizeObserver(() => onResize()) : null;
    if (ro) ro.observe(canvas.parentElement ?? canvas);
    if (typeof window !== "undefined") window.addEventListener("resize", onResize);
    return () => {
      canvas.removeEventListener("wheel", handleNativeWheel);
      ro?.disconnect();
      if (typeof window !== "undefined") window.removeEventListener("resize", onResize);
    };
  }, []);

  React.useEffect(() => {
    const canvas = canvasRef.current, ctx = canvas?.getContext("2d");
    if (!canvas || !ctx || !tileCacheRef.current) return;
    const parentRect = canvas.parentElement?.getBoundingClientRect();
    const rect = parentRect && parentRect.width > 0 && parentRect.height > 0 ? parentRect : canvas.getBoundingClientRect();
    const { width, height } = resolveNativeCanvasRenderArea(rect.width || 960, rect.height || 540);
    if (canvas.width !== width || canvas.height !== height) { canvas.width = width; canvas.height = height; }
    drawCanvasScene(ctx, { width, height }, mapMeta, storeSnap, tileCacheRef.current);
  });
}

function renderUnifiedInspectorSlot(
  storeSnap: WorldStreamState,
  viewState: ReturnType<typeof resolveVerraMapViewState>,
  onEntitySelect?: (entity: WorldEntity | null) => void,
): React.ReactElement | null {
  const selectedEntity = storeSnap.selectedEntityGuid ? storeSnap.entities.get(storeSnap.selectedEntityGuid) ?? null : null;
  const selectedCityNode = storeSnap.selectedCityNodeName ? storeSnap.cityNodes.find((n) => n.name === storeSnap.selectedCityNodeName) ?? null : null;
  const selectedPoi = storeSnap.selectedPoiGuid ? storeSnap.pois.find((p) => p.guid === storeSnap.selectedPoiGuid) ?? null : null;
  const pin = storeSnap.lastClickedPoint ?? storeSnap.pickedCoordinate;
  const inspectedEntity: WorldEntity | null = selectedEntity
    ? selectedEntity
    : selectedPoi
      ? poiOverlayToInspectedEntity(selectedPoi, storeSnap.workers)
      : null;
  if (!selectedCityNode && !inspectedEntity && !pin) return null;
  return (
    <div
      data-testid="unified-map-inspector-slot"
      style={{ position: "absolute", top: "12px", right: "12px", width: "360px", maxHeight: "calc(100% - 24px)", overflowY: "auto", zIndex: 25, display: "flex", flexDirection: "column", gap: "8px" }}
    >
      {selectedCityNode ? (
        <NodeGovernanceModalCard storeSnap={storeSnap} />
      ) : inspectedEntity ? (
        <>
          <EntityInspector
            viewState={viewState}
            entity={{
              ...inspectedEntity,
              xCm: Number(inspectedEntity.xCm ?? 0),
              yCm: Number(inspectedEntity.yCm ?? 0),
              zCm: Number(inspectedEntity.zCm ?? 0),
              healthPct: Number(inspectedEntity.healthPct ?? 100),
            }}
            onClose={() => {
              storeSnap.selectEntity(null);
              storeSnap.selectPoi(null);
              onEntitySelect?.(null);
            }}
          />
          <SelectedEntityQuickActions entity={inspectedEntity} storeSnap={storeSnap} />
        </>
      ) : pin ? (
        renderPickedCoordinateCard(pin, storeSnap)
      ) : null}
    </div>
  );
}

export function VerraMapCanvas(props: VerraMapCanvasProps): React.ReactElement {
  const storeSnap = useWorldStreamStore();
  const canvasRef = React.useRef<HTMLCanvasElement>(null);
  const dragRef = React.useRef<{ active: boolean; x: number; y: number; moved: boolean }>({ active: false, x: 0, y: 0, moved: false });
  const [mapMeta, setMapMeta] = React.useState<VerraMapMeta>(DEFAULT_VERRA_MAP_META);
  const [searchText, setSearchText] = React.useState("");
  const [cityFilterText, setCityFilterText] = React.useState("");
  const [, setRenderTick] = React.useState(0);
  const tileCacheRef = React.useRef<VerraTileCache | null>(null);
  if (!tileCacheRef.current) {
    tileCacheRef.current = new VerraTileCache(() => setRenderTick((t) => t + 1));
  }

  useWorldMapPolling(storeSnap, setMapMeta, canvasRef);
  useCanvasNativeWheelAndDraw(canvasRef, mapMeta, storeSnap, tileCacheRef, () => setRenderTick((t) => t + 1));
  const remoteSearchHits = useMapRemoteSearch(searchText, storeSnap.layers);
  const viewState = resolveVerraMapViewState(storeSnap);
  const deckLayers = buildVerraDeckOverlayLayers(storeSnap);
  const searchResults = filterMapSearchItems(searchText, storeSnap.cityNodes, storeSnap.entities, remoteSearchHits, storeSnap.pois);

  return (
    <section
      aria-label="Verra Spatial Telemetry Workspace"
      data-testid="verra-map-workspace"
      data-view-state={viewState}
      data-pmtiles-source={props.pmtilesUrl ?? DEFAULT_VERRA_PMTILES_URL}
      style={{ position: "relative", flex: 1, minHeight: 0, display: "flex", flexDirection: "column", width: props.width ?? "100%", height: props.height ?? "100%", maxHeight: "100%", boxSizing: "border-box", backgroundColor: SANDAL_LAUNCHER_TOKENS.VerraObsidian100, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, overflow: "hidden" }}
    >
      {renderMapHeader(searchText, searchResults, storeSnap, setSearchText, (hit) => {
        storeSnap.setCameraViewport({ centerXCm: hit.xCm, centerYCm: hit.yCm, zoom: Math.max(5.5, storeSnap.camera.zoom) });
        if (hit.playerName) storeSnap.setSelectedPlayer(hit.playerName);
        if (hit.nodeName) storeSnap.selectCityNode(hit.nodeName);
        else if (hit.entityGuid) storeSnap.selectEntity(hit.entityGuid);
        else if (hit.poiGuid) storeSnap.selectPoi(hit.poiGuid);
        setSearchText("");
      })}
      <div style={{ position: "relative", zIndex: 1, flex: 1, minHeight: 0, height: "100%", maxHeight: "100%", boxSizing: "border-box", overflow: "hidden" }}>
        {renderInteractiveMapCanvas(canvasRef, dragRef, storeSnap, deckLayers.filter((l) => l.visible).length, props.onCoordinatePick, props.onEntitySelect)}
        {renderZoomAndNodeControls(storeSnap, cityFilterText, setCityFilterText)}
        {renderBottomLeftMapEntityLegend()}
        {renderUnifiedInspectorSlot(storeSnap, viewState, props.onEntitySelect)}
        {renderBottomRightToastAndWorkersDock(storeSnap, canvasRef)}
        <GmCommandPalette isOpen={Boolean(props.isCommandPaletteOpen)} query={props.commandQuery ?? ""} role={props.gmRole ?? "GM_OPERATOR"} />
      </div>
    </section>
  );
}

export default VerraMapCanvas;
