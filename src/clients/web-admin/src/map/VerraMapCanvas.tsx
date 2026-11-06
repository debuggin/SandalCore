// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import { EntityInspector } from "../components/EntityInspector";
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
  renderNodeGovernanceModalCard,
  SANDAL_LAUNCHER_TOKENS,
} from "../components/PlayersAndSpawnerTab";
import { StreamStatus } from "../components/StreamStatus";
import {
  buildActiveLayersParam,
  buildWorldStatePollUrl,
  computeLayerEntityCounts,
  computeStreamLayerMask,
  DEFAULT_VERRA_CITY_NODES,
  DEFAULT_VERRA_MAP_META,
  getCityNodeCenterCm,
  type LayerToggles,
  type NpcInstanceAction,
  OVERLAY_LAYER_KEYS,
  resolveWorkerForCoordinateOrZone,
  shouldFetchPoisCatalog,
  useWorldStreamStore,
  VERRA_MAP_SOURCE_CM,
  VERRA_MAP_SOURCE_PX,
  type VerraMapMeta,
  type VerraUnrealPoint,
  type WorldEntity,
  type WorldStreamState,
} from "../state/worldStreamStore";
import {
  buildVerraDeckOverlayLayers,
  DEFAULT_VERRA_PMTILES_URL,
  drawCanvasScene,
  filterMapSearchItems,
  findNearestEntityOrNodeAtScreen,
  formatOverlayLayerLabel,
  handleCanvasRightClickClear,
  isEntityVisibleForLayers,
  type MapSearchResultItem,
  MAX_VERRA_MAP_ZOOM,
  MIN_VERRA_MAP_ZOOM,
  parsePoisFromPayload,
  PURE_BLACK_TILE_BG,
  resolveVerraMapViewState,
  screenOffsetToWorldCm,
  VerraTileCache,
} from "./verraTilePyramidRenderer";

export * from "../state/worldStreamStore";
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

function useWorldMapPolling(storeSnap: WorldStreamState, onMetaLoaded: (meta: VerraMapMeta) => void): void {
  const activeLayersParam = buildActiveLayersParam(storeSnap.layers);
  React.useEffect(() => {
    if (typeof window === "undefined" || typeof fetch !== "function") return;
    let cancelled = false;
    Promise.all([
      fetchWithTransientRetry("/api/v1/world/map-meta").then((r) => (r.ok ? r.json() : null)).catch(() => null),
      fetchWithTransientRetry("/api/v1/world/tiles/metadata.json").then((r) => (r.ok ? r.json() : null)).catch(() => null),
    ]).then(([metaData, tileMeta]) => {
      if (cancelled) return;
      const merged = { ...(tileMeta ?? {}), ...(metaData ?? {}) } as Partial<VerraMapMeta>;
      const nodes = Array.isArray(merged.nodes) && merged.nodes.length > 0 ? merged.nodes : DEFAULT_VERRA_CITY_NODES;
      onMetaLoaded({ ...DEFAULT_VERRA_MAP_META, ...merged, nodes });
      storeSnap.setCityNodes(nodes);
      storeSnap.setConnectionState("live");
    });
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
          const pois = parsePoisFromPayload(stateData);
          if (pois.length > 0) storeSnap.setPois(pois);
          storeSnap.applyWorldDelta({ seq: storeSnap.lastSeq + 1, isSnapshot: true, added });
        })
        .catch(() => {});
    };
    poll();
    const timer = window.setInterval(poll, 1000);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, [activeLayersParam]);
}

export function parseWorldStateResponse(raw: unknown, layers: LayerToggles): readonly WorldEntity[] {
  const obj = (raw && typeof raw === "object" ? raw : {}) as Record<string, unknown>;
  const added: WorldEntity[] = [];
  if (layers.players) {
    for (const p of normalizePaginatedEnvelope<Record<string, unknown>>(obj.players ?? obj, ["players"]).items) {
      added.push({
        guid: String(p.character_id ?? p.guid ?? p.name ?? "player"),
        name: String(p.name ?? "Player"),
        kind: "player",
        xCm: Number(p.x_cm ?? p.x ?? 0),
        yCm: Number(p.y_cm ?? p.y ?? 0),
        zCm: Number(p.z_cm ?? p.z ?? 0),
        yawDeg: Number(p.yaw_deg ?? p.yaw_degrees ?? 0),
        level: Number(p.level ?? 1),
        healthPct: 100,
      });
    }
  }
  const rawEntities = (Array.isArray(obj.entities) ? obj.entities : []) as readonly Record<string, unknown>[];
  for (const e of rawEntities) {
    const rawKind = String(e.kind ?? (e.hostile ? "monster" : "npc"));
    const kind =
      rawKind === "player" ||
      rawKind === "monster" ||
      rawKind === "resource_node" ||
      rawKind === "mount" ||
      rawKind === "loot_pile" ||
      rawKind === "poi"
        ? rawKind
        : "npc";
    const ent: WorldEntity = {
      guid: String(e.id ?? e.guid ?? e.name ?? "entity"),
      name: String(e.name ?? kind),
      kind,
      xCm: Number(e.x_cm ?? e.x ?? 0),
      yCm: Number(e.y_cm ?? e.y ?? 0),
      zCm: Number(e.z_cm ?? e.z ?? 0),
      yawDeg: Number(e.yaw_deg ?? 0),
      level: Number(e.level ?? 1),
      healthPct: Number(e.health_pct ?? e.hp ?? 100),
      npc_subkind: typeof e.npc_subkind === "string" ? e.npc_subkind : undefined,
      zoneId:
        typeof e.zone === "string"
          ? e.zone
          : typeof e.zone_id === "string"
            ? e.zone_id
            : undefined,
    };
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

function renderMapHeader(
  searchText: string,
  searchResults: readonly MapSearchResultItem[],
  storeSnap: WorldStreamState,
  onSearchChange: (next: string) => void,
  onSelectHit: (hit: MapSearchResultItem) => void,
): React.ReactElement {
  const counts = computeLayerEntityCounts(storeSnap);
  return (
    <header style={{ display: "flex", flexWrap: "wrap", alignItems: "center", justifyContent: "space-between", gap: "8px", padding: "8px 14px", background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush, backdropFilter: "blur(16px)", borderBottom: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}` }}>
      <StreamStatus />
      <div style={{ position: "relative", minWidth: "240px" }}>
        <input
          type="search"
          aria-label="Search online players on map"
          data-testid="verra-map-search-input"
          value={searchText}
          onChange={(e) => onSearchChange(e.currentTarget.value)}
          placeholder="Search online players on map..."
          style={{ width: "100%", minHeight: "32px", padding: "5px 10px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "12px" }}
        />
        {searchResults.length > 0 ? (
          <ul role="listbox" aria-label="Player search results" data-testid="verra-map-search-results" style={{ position: "absolute", top: "36px", left: 0, right: 0, maxHeight: "240px", overflowY: "auto", zIndex: 40, margin: 0, padding: "4px", listStyle: "none", borderRadius: "6px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.VerraBorder400}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.VerraObsidian100 }}>
            {searchResults.map((hit) => (
              <li key={hit.id}>
                <button type="button" onClick={() => onSelectHit(hit)} style={{ width: "100%", minHeight: "32px", padding: "4px 8px", textAlign: "left", border: "none", background: "transparent", color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500, fontSize: "12px", cursor: "pointer" }}>
                  {hit.label}
                </button>
              </li>
            ))}
          </ul>
        ) : null}
      </div>
      <GmActionModalsDock />
      <div role="toolbar" aria-label="Map layer visibility toggles" style={{ display: "flex", gap: "5px", flexWrap: "wrap" }}>
        {OVERLAY_LAYER_KEYS.map((layerKey) => (
          <button
            key={layerKey}
            type="button"
            data-testid={`layer-toggle-${layerKey}`}
            aria-pressed={storeSnap.layers[layerKey]}
            onClick={() => storeSnap.toggleLayer(layerKey)}
            style={{
              minHeight: "32px",
              padding: "4px 10px",
              borderRadius: "4px",
              border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
              backgroundColor: storeSnap.layers[layerKey] ? SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush,
              color: storeSnap.layers[layerKey] ? SANDAL_LAUNCHER_TOKENS.VerraObsidian100 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100,
              fontSize: "11px",
              fontWeight: 700,
              cursor: "pointer",
            }}
          >
            {formatOverlayLayerLabel(layerKey, counts)}
          </button>
        ))}
        <button
          type="button"
          data-testid="layer-toggle-zoiBorders"
          aria-pressed={storeSnap.layers.zoiBorders}
          onClick={() => storeSnap.toggleLayer("zoiBorders")}
          style={{
            minHeight: "32px",
            padding: "4px 10px",
            borderRadius: "4px",
            border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
            backgroundColor: storeSnap.layers.zoiBorders
              ? SANDAL_LAUNCHER_TOKENS.VerraRelicGold500
              : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush,
            color: storeSnap.layers.zoiBorders
              ? SANDAL_LAUNCHER_TOKENS.VerraObsidian100
              : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100,
            fontSize: "11px",
            fontWeight: 700,
            cursor: "pointer",
          }}
        >
          ZOI Borders
        </button>
        <button
          type="button"
          data-testid="toggle-simulate-all-npcs"
          aria-pressed={storeSnap.simulateAllNpcs}
          onClick={() => void storeSnap.setSimulateAllNpcs(!storeSnap.simulateAllNpcs)}
          style={{
            minHeight: "32px",
            padding: "4px 10px",
            borderRadius: "4px",
            border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
            backgroundColor: storeSnap.simulateAllNpcs
              ? SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500
              : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush,
            color: storeSnap.simulateAllNpcs
              ? SANDAL_LAUNCHER_TOKENS.VerraObsidian100
              : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100,
            fontSize: "11px",
            fontWeight: 700,
            cursor: "pointer",
          }}
        >
          Sim All NPCs: {storeSnap.simulateAllNpcs ? "ON" : "OFF"}
        </button>
      </div>
    </header>
  );
}

function renderZoomAndNodeControls(storeSnap: WorldStreamState): React.ReactElement {
  const pin = storeSnap.lastClickedPoint ?? storeSnap.pickedCoordinate;
  const selectedNode = storeSnap.selectedCityNodeName
    ? storeSnap.cityNodes.find((n) => n.name === storeSnap.selectedCityNodeName) ?? null
    : null;
  const selectedEntity = storeSnap.selectedEntityGuid
    ? storeSnap.entities.get(storeSnap.selectedEntityGuid) ?? null
    : null;
  const nodeCenter = selectedNode ? getCityNodeCenterCm(selectedNode) : null;
  const targetX = selectedEntity
    ? selectedEntity.xCm
    : nodeCenter
      ? nodeCenter.cx_cm
      : Number(pin?.xCm ?? storeSnap.camera.centerXCm);
  const targetY = selectedEntity
    ? selectedEntity.yCm
    : nodeCenter
      ? nodeCenter.cy_cm
      : Number(pin?.yCm ?? storeSnap.camera.centerYCm);
  const worker = resolveWorkerForCoordinateOrZone(
    targetX,
    targetY,
    selectedEntity?.zoneId,
    undefined,
    selectedNode?.name,
  );
  return (
    <div style={{ position: "absolute", right: "12px", top: "12px", display: "flex", flexDirection: "column", gap: "4px", zIndex: 20 }}>
      <button type="button" aria-label="Zoom in" data-testid="map-zoom-in" onClick={() => storeSnap.setCameraViewport({ zoom: Math.min(MAX_VERRA_MAP_ZOOM, storeSnap.camera.zoom + 1) })} style={{ width: "32px", height: "32px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, cursor: "pointer" }}>+</button>
      <button type="button" aria-label="Zoom out" data-testid="map-zoom-out" onClick={() => storeSnap.setCameraViewport({ zoom: Math.max(MIN_VERRA_MAP_ZOOM, storeSnap.camera.zoom - 1) })} style={{ width: "32px", height: "32px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, cursor: "pointer" }}>-</button>
      <select
        aria-label="Select City Node to inspect or change stage"
        data-testid="city-node-quick-select"
        value={storeSnap.selectedCityNodeName ?? ""}
        onChange={(e) => {
          const name = e.currentTarget.value || null;
          storeSnap.selectCityNode(name);
          const found = storeSnap.cityNodes.find((n) => n.name === name);
          if (found) {
            const { cx_cm, cy_cm } = getCityNodeCenterCm(found);
            storeSnap.setCameraViewport({ centerXCm: cx_cm, centerYCm: cy_cm, zoom: 5.5 });
          }
        }}
        style={{ minHeight: "32px", padding: "4px 6px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "11px" }}
      >
        <option value="">26 City Nodes...</option>
        {storeSnap.cityNodes.map((n) => (
          <option key={n.name} value={n.name}>{n.display_name} (Lv {n.level})</option>
        ))}
      </select>
      <div
        data-testid="map-authority-worker-badge"
        style={{
          padding: "5px 8px",
          borderRadius: "6px",
          border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
          background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush,
          color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400,
          fontSize: "11px",
          fontWeight: 600,
        }}
      >
        {worker.worker_id} · {worker.region_label}
      </div>
      {pin ? (
        <div data-testid="picked-coordinate-readout" style={{ padding: "6px 8px", borderRadius: "6px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "11px" }}>
          Pin: ({Number(pin.xCm ?? 0).toFixed(0)}, {Number(pin.yCm ?? 0).toFixed(0)}) cm
        </div>
      ) : null}
    </div>
  );
}

function handleCanvasLeftClick(
  e: React.MouseEvent<HTMLCanvasElement>,
  storeSnap: WorldStreamState,
  onCoordinatePick?: (point: VerraUnrealPoint) => void,
): void {
  const rect = e.currentTarget.getBoundingClientRect();
  const sx = e.clientX - rect.left;
  const sy = e.clientY - rect.top;
  const viewport = { width: rect.width || 960, height: rect.height || 540 };
  const hit = findNearestEntityOrNodeAtScreen(sx, sy, viewport, storeSnap);
  if (hit?.entity) {
    storeSnap.selectEntity(hit.entity.guid);
    return;
  }
  if (hit?.node) {
    storeSnap.selectCityNode(hit.node.name);
    return;
  }
  if (hit?.poi) {
    storeSnap.selectPoi(hit.poi.guid);
    storeSnap.setPickedCoordinate({ xCm: hit.poi.x_cm, yCm: hit.poi.y_cm, zCm: hit.poi.z_cm });
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
      onKeyDown={(e) => {
        if (e.key === "Escape") handleCanvasRightClickClear(storeSnap);
      }}
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
        if (e.button === 0 && !wasMoved) handleCanvasLeftClick(e, storeSnap, onCoordinatePick);
      }}
      onContextMenu={(e) => {
        e.preventDefault();
        e.stopPropagation();
        handleCanvasRightClickClear(storeSnap);
      }}
      style={{ display: "block", width: "100%", height: "100%", cursor: "grab", backgroundColor: PURE_BLACK_TILE_BG }}
    ></canvas>
  );
}

function renderSelectedEntityQuickActions(entity: WorldEntity, storeSnap: WorldStreamState): React.ReactElement {
  const playerX = storeSnap.selectedPlayer;
  const npcTemplate = storeSnap.selectedNpcTemplate || "DesignData_NPC_Goblin";
  const worker = resolveWorkerForCoordinateOrZone(entity.xCm, entity.yCm, entity.zoneId, undefined, entity.name);
  const tpReceipt = storeSnap.lastReceiptsByScope.teleport;
  const spawnReceipt = storeSnap.lastReceiptsByScope.spawnNpc;
  const entReceipt = storeSnap.lastReceiptsByScope.entityAction;
  return (
    <div data-testid="selected-entity-quick-actions" style={{ position: "relative", width: "100%", boxSizing: "border-box", padding: "10px 12px", borderRadius: "6px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush, backdropFilter: "blur(16px)", color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "12px", zIndex: 20 }}>
      <div style={{ display: "flex", gap: "4px", flexWrap: "wrap", marginBottom: "6px" }}>
        {NPC_INSTANCE_ACTIONS.map((act) => (
          <button
            key={act.id}
            type="button"
            data-testid={`npc-instance-${act.id}`}
            onClick={() =>
              void dispatchAuxiliaryGmAction({
                action: act.id,
                entity_guid: entity.guid,
                zone: worker.scope_key,
                worker_id: worker.worker_id,
              }).then((r) =>
                storeSnap.setScopedReceipt("entityAction", { ok: r.ok, message: `${entity.name} (${act.label}): ${r.message}` }),
              )
            }
            style={{ minHeight: "32px", padding: "4px 8px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "11px", cursor: "pointer" }}
          >
            {act.label}
          </button>
        ))}
      </div>
      <div style={{ display: "flex", gap: "6px", flexWrap: "wrap" }}>
        <button
          type="button"
          data-testid="inspector-teleport-here-btn"
          onClick={() => {
            if (!playerX) return;
            void sendGmCommand({
              ...buildTeleportToCoordsCommand(playerX, entity.xCm, entity.yCm, entity.zCm),
              zone: worker.scope_key,
              worker_id: worker.worker_id,
            } as never).then((r) =>
              storeSnap.setScopedReceipt("teleport", { ok: r.ok, message: r.message, correlationId: r.correlationId }),
            );
          }}
          style={{ minHeight: "32px", padding: "4px 8px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.VerraBorder400}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500, color: SANDAL_LAUNCHER_TOKENS.VerraObsidian100, fontWeight: 700, fontSize: "11px", cursor: "pointer" }}
        >
          Teleport {playerX || "Player X"} Here
        </button>
        <button
          type="button"
          data-testid="inspector-spawn-next-btn"
          onClick={() => {
            void sendGmCommand({
              ...buildSpawnNpcCommand({ npc: npcTemplate, x_cm: entity.xCm + 150, y_cm: entity.yCm + 150, z_cm: entity.zCm, level: entity.level || 15 }),
              zone: worker.scope_key,
              worker_id: worker.worker_id,
            } as never).then((r) =>
              storeSnap.setScopedReceipt("spawnNpc", { ok: r.ok, message: r.message, correlationId: r.correlationId }),
            );
          }}
          style={{ minHeight: "32px", padding: "4px 8px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "11px", cursor: "pointer" }}
        >
          Spawn {npcTemplate} Here
        </button>
      </div>
      {entReceipt ? <div data-testid="inspector-entity-action-receipt" style={{ marginTop: "4px", color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400, fontSize: "11px" }}>{entReceipt.message}</div> : null}
      {tpReceipt ? <div data-testid="inspector-teleport-receipt" style={{ marginTop: "4px", color: SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500, fontSize: "11px" }}>{tpReceipt.message}</div> : null}
      {spawnReceipt ? <div data-testid="inspector-spawn-receipt" style={{ marginTop: "4px", color: SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500, fontSize: "11px" }}>{spawnReceipt.message}</div> : null}
    </div>
  );
}

function useCanvasNativeWheelAndDraw(
  canvasRef: React.RefObject<HTMLCanvasElement>,
  mapMeta: VerraMapMeta,
  storeSnap: WorldStreamState,
  tileCacheRef: React.MutableRefObject<VerraTileCache | null>,
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
      const nextZoom = Math.max(
        MIN_VERRA_MAP_ZOOM,
        Math.min(MAX_VERRA_MAP_ZOOM, snap.camera.zoom + (ev.deltaY < 0 ? 0.5 : -0.5)),
      );
      snap.setCameraViewport({ zoom: nextZoom });
    };
    canvas.addEventListener("wheel", handleNativeWheel, { passive: false });
    return () => {
      canvas.removeEventListener("wheel", handleNativeWheel);
    };
  }, []);

  React.useEffect(() => {
    const canvas = canvasRef.current;
    const ctx = canvas?.getContext("2d");
    if (!canvas || !ctx || !tileCacheRef.current) return;
    const rect = canvas.getBoundingClientRect();
    const width = Math.max(320, Math.round(rect.width || 960));
    const height = Math.max(240, Math.round(rect.height || 540));
    if (canvas.width !== width || canvas.height !== height) {
      canvas.width = width;
      canvas.height = height;
    }
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
  const inspectedEntity: WorldEntity | null = selectedEntity
    ? selectedEntity
    : selectedPoi
      ? {
          guid: selectedPoi.guid,
          name: selectedPoi.display_name || selectedPoi.name,
          kind: "poi",
          xCm: selectedPoi.x_cm,
          yCm: selectedPoi.y_cm,
          zCm: selectedPoi.z_cm,
          yawDeg: 0,
          level: 1,
          healthPct: 100,
        }
      : null;
  if (!selectedCityNode && !inspectedEntity) return null;
  return (
    <div
      data-testid="unified-map-inspector-slot"
      style={{ position: "absolute", top: "56px", right: "12px", width: "360px", zIndex: 25, display: "flex", flexDirection: "column", gap: "8px" }}
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
          {selectedEntity ? renderSelectedEntityQuickActions(selectedEntity, storeSnap) : null}
        </>
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
  const [, setRenderTick] = React.useState(0);
  const tileCacheRef = React.useRef<VerraTileCache | null>(null);
  if (!tileCacheRef.current) {
    tileCacheRef.current = new VerraTileCache(() => setRenderTick((t) => t + 1));
  }

  useWorldMapPolling(storeSnap, setMapMeta);
  useCanvasNativeWheelAndDraw(canvasRef, mapMeta, storeSnap, tileCacheRef);
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
      style={{ position: "relative", display: "flex", flexDirection: "column", width: props.width ?? "100%", height: props.height ?? "100%", minHeight: "540px", backgroundColor: SANDAL_LAUNCHER_TOKENS.VerraObsidian100, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, overflow: "hidden" }}
    >
      {renderMapHeader(searchText, searchResults, storeSnap, setSearchText, (hit) => {
        storeSnap.setCameraViewport({ centerXCm: hit.xCm, centerYCm: hit.yCm, zoom: Math.max(5.5, storeSnap.camera.zoom) });
        if (hit.playerName) storeSnap.setSelectedPlayer(hit.playerName);
        if (hit.nodeName) storeSnap.selectCityNode(hit.nodeName);
        else if (hit.entityGuid) storeSnap.selectEntity(hit.entityGuid);
        else if (hit.poiGuid) storeSnap.selectPoi(hit.poiGuid);
        setSearchText("");
      })}
      <div style={{ position: "relative", flex: 1, minHeight: "460px" }}>
        {renderInteractiveMapCanvas(canvasRef, dragRef, storeSnap, deckLayers.filter((l) => l.visible).length, props.onCoordinatePick)}
        {renderZoomAndNodeControls(storeSnap)}
        {renderUnifiedInspectorSlot(storeSnap, viewState, props.onEntitySelect)}
        <GmCommandPalette isOpen={Boolean(props.isCommandPaletteOpen)} query={props.commandQuery ?? ""} role={props.gmRole ?? "GM_OPERATOR"} />
      </div>
    </section>
  );
}

export default VerraMapCanvas;
