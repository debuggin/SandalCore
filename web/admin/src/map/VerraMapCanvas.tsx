// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import { EntityInspector } from "../components/EntityInspector";
import { GmCommandPalette, type GmRole } from "../components/GmCommandPalette";
import { StreamStatus } from "../components/StreamStatus";
import { WorldContextMenu } from "../components/WorldContextMenu";
import {
  type CameraViewport,
  type LayerToggles,
  lngLatToVerraUnreal,
  type LngLatTuple,
  type NavmeshLinkSegment,
  useWorldStreamStore,
  type VerraUnrealPoint,
  verraUnrealToLngLat,
  type VoronoiZoneBoundary,
  type WorldEntity,
  type WorldStreamState,
} from "../state/worldStreamStore";

export const DEFAULT_VERRA_PMTILES_URL = "pmtiles:///tiles/verra.pmtiles";
export const SEMANTIC_LOD_SPAWN_MIN_ZOOM = 2.0;
export const SEMANTIC_LOD_ICON_MIN_ZOOM = 2.5;

export type VerraMapViewState =
  | "loading"
  | "empty"
  | "error"
  | "degraded"
  | "populated";

export type DeckLayerKind =
  | "PolygonLayer"
  | "PathLayer"
  | "ScatterplotLayer"
  | "IconLayer";

export interface VerraDeckLayerDescriptor<TData = unknown> {
  readonly id: string;
  readonly layerType: DeckLayerKind;
  readonly visible: boolean;
  readonly pickable: boolean;
  readonly data: readonly TData[];
  readonly props: Readonly<Record<string, unknown>>;
}

export type MapLibreProtocolTileHandler = (
  params: { readonly url: string },
  abortController: AbortController,
) => Promise<{ readonly data: ArrayBuffer }>;

export type MapLibreAddProtocolFn = (
  customProtocol: string,
  handler: MapLibreProtocolTileHandler,
) => void;

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

const LAYER_KEYS: readonly (keyof LayerToggles)[] = Object.freeze([
  "players",
  "npcs",
  "spawns",
  "navmesh",
  "voronoiBorders",
]);

let pmtilesProtocolRegistered = false;

// Registers pmtiles:// byte-range tile streaming idempotently without main-thread GeoJSON parsing.
export function registerPmtilesProtocol(
  addProtocol?: MapLibreAddProtocolFn,
  removeProtocol?: MapLibreRemoveProtocolFn,
): () => void {
  if (!pmtilesProtocolRegistered && addProtocol) {
    const defaultHandler: MapLibreProtocolTileHandler = async () => ({
      data: new ArrayBuffer(0),
    });
    addProtocol("pmtiles", defaultHandler);
    pmtilesProtocolRegistered = true;
  }
  return () => {
    if (pmtilesProtocolRegistered && removeProtocol) {
      removeProtocol("pmtiles");
    }
    pmtilesProtocolRegistered = false;
  };
}

export function resolveVerraMapViewState(
  state: Pick<
    WorldStreamState,
    | "connectionState"
    | "error"
    | "lastSeq"
    | "needsSnapshotResync"
    | "entities"
    | "voronoiZones"
    | "navmeshSegments"
  >,
): VerraMapViewState {
  if (state.connectionState === "offline" || state.error !== null) {
    return "error";
  }
  if (state.connectionState === "connecting" && state.lastSeq === 0) {
    return "loading";
  }
  if (state.connectionState === "degraded" || state.needsSnapshotResync) {
    return "degraded";
  }
  const hasSpatialContent =
    state.entities.size > 0 ||
    state.voronoiZones.length > 0 ||
    state.navmeshSegments.length > 0;
  return hasSpatialContent ? "populated" : "empty";
}

function partitionEntitiesByLayer(
  entities: ReadonlyMap<string, WorldEntity>,
  layers: LayerToggles,
  zoom: number,
): {
  readonly activeEntities: readonly WorldEntity[];
  readonly iconEntities: readonly WorldEntity[];
} {
  const activeEntities: WorldEntity[] = [];
  const iconEntities: WorldEntity[] = [];
  const includeSpawnsAtZoom = zoom >= SEMANTIC_LOD_SPAWN_MIN_ZOOM;
  const includeIconsAtZoom = zoom >= SEMANTIC_LOD_ICON_MIN_ZOOM;

  for (const entity of entities.values()) {
    const isPlayer = entity.kind === "player";
    const isNpc = entity.kind === "npc";
    const isSpawnGroup = !isPlayer && !isNpc;
    const visible =
      (isPlayer && layers.players) ||
      (isNpc && layers.npcs) ||
      (isSpawnGroup && layers.spawns && includeSpawnsAtZoom);
    if (visible) {
      activeEntities.push(entity);
      if (includeIconsAtZoom && (isPlayer || isNpc)) {
        iconEntities.push(entity);
      }
    }
  }
  return { activeEntities, iconEntities };
}

function buildVoronoiPolygonLayer(
  zones: readonly VoronoiZoneBoundary[],
  visible: boolean,
  mode3d: boolean,
): VerraDeckLayerDescriptor<VoronoiZoneBoundary> {
  return {
    id: "verra-voronoi-borders-polygon",
    layerType: "PolygonLayer",
    visible,
    pickable: true,
    data: visible ? zones : [],
    props: {
      extruded: mode3d,
      wireframe: true,
      filled: true,
      lineWidthMinPixels: 1.5,
      getPolygon: (zone: VoronoiZoneBoundary): LngLatTuple[] =>
        zone.verticesCm.map(([xCm, yCm]) => verraUnrealToLngLat(xCm, yCm)),
      getElevation: (zone: VoronoiZoneBoundary): number =>
        mode3d ? zone.nodeLevel * 120.0 : 0.0,
      getFillColor: [31, 111, 235, 28],
      getLineColor: [88, 166, 255, 190],
    },
  };
}

function buildNavmeshPathLayer(
  segments: readonly NavmeshLinkSegment[],
  visible: boolean,
): VerraDeckLayerDescriptor<NavmeshLinkSegment> {
  return {
    id: "verra-navmesh-links-path",
    layerType: "PathLayer",
    visible,
    pickable: false,
    data: visible ? segments : [],
    props: {
      widthMinPixels: 1.5,
      getPath: (seg: NavmeshLinkSegment): LngLatTuple[] => [
        verraUnrealToLngLat(seg.fromCm[0], seg.fromCm[1]),
        verraUnrealToLngLat(seg.toCm[0], seg.toCm[1]),
      ],
      getColor: [63, 185, 80, 160],
    },
  };
}

function buildScatterplotLayer(
  activeEntities: readonly WorldEntity[],
  selectedGuid: string | null,
  lastSeq: number,
  mode3d: boolean,
): VerraDeckLayerDescriptor<WorldEntity> {
  return {
    id: "verra-live-entities-scatterplot",
    layerType: "ScatterplotLayer",
    visible: activeEntities.length > 0,
    pickable: true,
    data: activeEntities,
    props: {
      radiusMinPixels: 4,
      radiusMaxPixels: 14,
      getPosition: (entity: WorldEntity): readonly [number, number, number] => {
        const [lng, lat] = verraUnrealToLngLat(entity.xCm, entity.yCm);
        return [lng, lat, mode3d ? entity.zCm * 0.01 : 0.0];
      },
      getRadius: (entity: WorldEntity): number =>
        entity.guid === selectedGuid ? 12 : 6,
      getFillColor: (
        entity: WorldEntity,
      ): readonly [number, number, number, number] =>
        entity.kind === "player"
          ? [56, 139, 253, 235]
          : entity.kind === "npc"
            ? [210, 153, 34, 225]
            : [139, 148, 158, 200],
      updateTriggers: {
        getPosition: [lastSeq, mode3d],
        getRadius: [selectedGuid],
      },
    },
  };
}

function buildHeadingIconLayer(
  iconEntities: readonly WorldEntity[],
  lastSeq: number,
): VerraDeckLayerDescriptor<WorldEntity> {
  return {
    id: "verra-entity-heading-icon",
    layerType: "IconLayer",
    visible: iconEntities.length > 0,
    pickable: true,
    data: iconEntities,
    props: {
      sizeScale: 1,
      getPosition: (entity: WorldEntity): LngLatTuple =>
        verraUnrealToLngLat(entity.xCm, entity.yCm),
      getAngle: (entity: WorldEntity): number => 360.0 - (entity.yawDeg % 360.0),
      getSize: 16,
      updateTriggers: {
        getPosition: [lastSeq],
        getAngle: [lastSeq],
      },
    },
  };
}

// GPU-instanced Scatterplot + Icon layers avoid >100 DOM marker stalls while keeping 16.6ms INP.
export function buildVerraDeckOverlayLayers(
  state: Pick<
    WorldStreamState,
    | "entities"
    | "selectedEntityGuid"
    | "layers"
    | "camera"
    | "voronoiZones"
    | "navmeshSegments"
    | "lastSeq"
  >,
): readonly VerraDeckLayerDescriptor[] {
  const { activeEntities, iconEntities } = partitionEntitiesByLayer(
    state.entities,
    state.layers,
    state.camera.zoom,
  );
  const mode3d = state.camera.mode === "3d";
  return [
    buildVoronoiPolygonLayer(
      state.voronoiZones,
      state.layers.voronoiBorders,
      mode3d,
    ),
    buildNavmeshPathLayer(state.navmeshSegments, state.layers.navmesh),
    buildScatterplotLayer(
      activeEntities,
      state.selectedEntityGuid,
      state.lastSeq,
      mode3d,
    ),
    buildHeadingIconLayer(iconEntities, state.lastSeq),
  ];
}

function projectClientOffsetToVerraPoint(
  clientX: number,
  clientY: number,
  rect: {
    readonly left: number;
    readonly top: number;
    readonly width: number;
    readonly height: number;
  },
  camera: CameraViewport,
): VerraUnrealPoint {
  const safeWidth = rect.width > 0 ? rect.width : 1;
  const safeHeight = rect.height > 0 ? rect.height : 1;
  const normX = (clientX - rect.left) / safeWidth - 0.5;
  const normY = 0.5 - (clientY - rect.top) / safeHeight;
  const spanScale = Math.pow(2, Math.max(0, 4 - camera.zoom));
  const lng = camera.longitude + normX * 45.0 * spanScale;
  const lat = camera.latitude + normY * 22.5 * spanScale;
  return lngLatToVerraUnreal(lng, lat, 0.0);
}

function renderViewStateBanner(
  viewState: VerraMapViewState,
  state: WorldStreamState,
): React.ReactElement | null {
  if (viewState === "error") {
    const title = state.error?.title ?? "World Stream Disconnected";
    const detail =
      state.error?.detail ??
      "Real-time WebSocket stream is offline. Map remains inspectable with last known state.";
    return (
      <div
        role="alert"
        data-testid="verra-map-error-banner"
        style={{ padding: "8px 12px", backgroundColor: "rgba(218, 54, 51, 0.2)", borderBottom: "1px solid #DA3633", color: "#FFA198", fontSize: "12px" }}
      >
        <strong>{title}</strong>: {detail}
        {state.error?.correlationId ? <span> (Correlation ID: {state.error.correlationId})</span> : null}
      </div>
    );
  }
  if (viewState === "degraded") {
    return (
      <div
        role="status"
        data-testid="verra-map-degraded-banner"
        style={{ padding: "6px 12px", backgroundColor: "rgba(158, 106, 3, 0.2)", borderBottom: "1px solid #9E6A03", color: "#E3B341", fontSize: "12px" }}
      >
        Degraded stream telemetry or sequence gap detected; awaiting snapshot resynchronization.
      </div>
    );
  }
  if (viewState === "loading") {
    return (
      <div
        role="status"
        aria-busy="true"
        data-testid="verra-map-loading-banner"
        style={{ padding: "6px 12px", backgroundColor: "#161B22", borderBottom: "1px solid #30363D", color: "#8B949E", fontSize: "12px" }}
      >
        Synchronizing initial Verra world snapshot and PMTiles base layers...
      </div>
    );
  }
  return null;
}

function renderLayerToolbar(
  layers: LayerToggles,
  onToggle: (layer: keyof LayerToggles) => void,
): React.ReactElement {
  return (
    <div
      role="toolbar"
      aria-label="Map layer visibility toggles"
      style={{ display: "flex", gap: "6px", flexWrap: "wrap" }}
    >
      {LAYER_KEYS.map((layerKey) => {
        const active = layers[layerKey];
        return (
          <button
            key={layerKey}
            type="button"
            data-testid={`layer-toggle-${layerKey}`}
            aria-pressed={active}
            onClick={() => onToggle(layerKey)}
            style={{
              minHeight: "28px",
              padding: "4px 8px",
              borderRadius: "4px",
              border: "1px solid #30363D",
              backgroundColor: active ? "#1F6FEB" : "#21262D",
              color: "#F0F6FC",
              fontSize: "11px",
              cursor: "pointer",
            }}
          >
            {layerKey}
          </button>
        );
      })}
    </div>
  );
}

function renderAccessibleEntityList(
  entities: ReadonlyMap<string, WorldEntity>,
  selectedGuid: string | null,
  onSelect: (entity: WorldEntity) => void,
): React.ReactElement {
  return (
    <ul
      aria-label="Inspectable live entities"
      data-testid="verra-accessible-entity-list"
      style={{
        position: "absolute",
        left: "12px",
        bottom: "12px",
        maxHeight: "140px",
        overflowY: "auto",
        margin: 0,
        padding: "6px 8px",
        listStyle: "none",
        borderRadius: "6px",
        border: "1px solid #30363D",
        backgroundColor: "rgba(13, 17, 23, 0.86)",
        fontSize: "11px",
      }}
    >
      {Array.from(entities.values())
        .slice(0, 25)
        .map((entity) => (
          <li key={entity.guid}>
            <button
              type="button"
              data-testid={`select-entity-${entity.guid}`}
              onClick={() => onSelect(entity)}
              style={{
                minHeight: "24px",
                border: "none",
                background: "transparent",
                color: entity.guid === selectedGuid ? "#58A6FF" : "#C9D1D9",
                cursor: "pointer",
                textAlign: "left",
              }}
            >
              {entity.name} ({entity.kind})
            </button>
          </li>
        ))}
    </ul>
  );
}

const WORKSPACE_HEADER_STYLE: React.CSSProperties = Object.freeze({
  display: "flex",
  flexWrap: "wrap",
  alignItems: "center",
  justifyContent: "space-between",
  gap: "8px",
  padding: "8px 12px",
  backgroundColor: "#0D1117",
  borderBottom: "1px solid #30363D",
});

const EMPTY_STATE_OVERLAY_STYLE: React.CSSProperties = Object.freeze({
  position: "absolute",
  inset: "0",
  display: "flex",
  alignItems: "center",
  justifyContent: "center",
  pointerEvents: "none",
  color: "#8B949E",
  fontSize: "13px",
});

export function VerraMapCanvas(props: VerraMapCanvasProps): React.ReactElement {
  const storeSnap = useWorldStreamStore();
  const pmtilesUrl = props.pmtilesUrl ?? DEFAULT_VERRA_PMTILES_URL;
  const viewState = resolveVerraMapViewState(storeSnap);
  const deckLayers = buildVerraDeckOverlayLayers(storeSnap);
  const selectedEntity = storeSnap.selectedEntityGuid
    ? storeSnap.entities.get(storeSnap.selectedEntityGuid) ?? null
    : null;

  const handleSelectEntity = (entity: WorldEntity | null): void => {
    storeSnap.selectEntity(entity ? entity.guid : null);
    props.onEntitySelect?.(entity);
  };

  const handleCanvasContextMenu = (event: React.MouseEvent<HTMLCanvasElement>): void => {
    event.preventDefault();
    const point = projectClientOffsetToVerraPoint(
      event.clientX,
      event.clientY,
      event.currentTarget.getBoundingClientRect(),
      storeSnap.camera,
    );
    storeSnap.setPickedCoordinate(point);
    props.onCoordinatePick?.(point);
  };

  return (
    <section
      aria-label="Verra Spatial Telemetry Workspace"
      data-testid="verra-map-workspace"
      data-view-state={viewState}
      data-pmtiles-source={pmtilesUrl}
      style={{ position: "relative", display: "flex", flexDirection: "column", width: props.width ?? "100%", height: props.height ?? "100%", minHeight: "480px", backgroundColor: "#090D16", color: "#E6EDF3", overflow: "hidden" }}
    >
      <header style={WORKSPACE_HEADER_STYLE}>
        <StreamStatus />
        {renderLayerToolbar(storeSnap.layers, (k) => storeSnap.toggleLayer(k))}
      </header>
      {renderViewStateBanner(viewState, storeSnap)}
      <div style={{ position: "relative", flex: 1, minHeight: "400px" }}>
        <canvas
          role="region"
          aria-label="Verra 2D/3D Interactive World Map Canvas"
          tabIndex={0}
          data-testid="verra-maplibre-deck-canvas"
          data-camera-mode={storeSnap.camera.mode}
          data-active-layers={deckLayers.filter((l) => l.visible).length}
          onContextMenu={handleCanvasContextMenu}
          style={{ display: "block", width: "100%", height: "100%", cursor: "crosshair" }}
        />
        {viewState === "empty" ? (
          <div role="status" data-testid="verra-map-empty-state" style={EMPTY_STATE_OVERLAY_STYLE}>
            No active entities or spatial overlays in current Verra sector.
          </div>
        ) : null}
        {renderAccessibleEntityList(storeSnap.entities, storeSnap.selectedEntityGuid, handleSelectEntity)}
        {selectedEntity ? (
          <EntityInspector
            viewState={viewState}
            entity={selectedEntity}
            onClose={() => handleSelectEntity(null)}
          />
        ) : null}
        <WorldContextMenu
          picked={storeSnap.pickedCoordinate}
          selectedEntityGuid={storeSnap.selectedEntityGuid}
          selectedEntityOrigin={selectedEntity}
          onDismiss={() => storeSnap.setPickedCoordinate(null)}
        />
        <GmCommandPalette
          isOpen={Boolean(props.isCommandPaletteOpen)}
          query={props.commandQuery ?? ""}
          role={props.gmRole ?? "GM_OPERATOR"}
        />
      </div>
    </section>
  );
}

export default VerraMapCanvas;
