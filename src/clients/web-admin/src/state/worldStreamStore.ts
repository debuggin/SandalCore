// SPDX-License-Identifier: AGPL-3.0-only

export const VERRA_EXTENT_CM = 1_200_000.0;
export const VERRA_MAX_LNG_DEG = 180.0;
export const VERRA_MAX_LAT_DEG = 85.05112877980659;
export const STREAM_TARGET_HZ = 10;
export const UI_FRAME_BUDGET_MS = 16.6;

export type LngLatTuple = readonly [number, number];

export interface VerraUnrealPoint {
  readonly xCm: number;
  readonly yCm: number;
  readonly zCm: number;
}

export type WorldEntityKind =
  | "player"
  | "npc"
  | "spawn"
  | "resource_node"
  | "mount"
  | "loot_pile";

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

export interface LayerToggles {
  readonly players: boolean;
  readonly npcs: boolean;
  readonly spawns: boolean;
  readonly navmesh: boolean;
  readonly voronoiBorders: boolean;
}

export type CameraMode = "2d" | "3d";

export interface CameraViewport {
  readonly longitude: number;
  readonly latitude: number;
  readonly zoom: number;
  readonly pitch: number;
  readonly bearing: number;
  readonly mode: CameraMode;
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

export interface VoronoiZoneBoundary {
  readonly zoneId: string;
  readonly displayName: string;
  readonly nodeLevel: number;
  readonly verticesCm: readonly (readonly [number, number])[];
}

export interface NavmeshLinkSegment {
  readonly segmentId: string;
  readonly fromCm: readonly [number, number, number];
  readonly toCm: readonly [number, number, number];
}

export interface WorldStreamState {
  readonly entities: ReadonlyMap<string, WorldEntity>;
  readonly selectedEntityGuid: string | null;
  readonly layers: LayerToggles;
  readonly camera: CameraViewport;
  readonly connectionState: StreamConnectionState;
  readonly lastSeq: number;
  readonly tickLatencyMs: number;
  readonly streamHz: number;
  readonly needsSnapshotResync: boolean;
  readonly error: StreamProblemDetails | null;
  readonly pickedCoordinate: VerraUnrealPoint | null;
  readonly voronoiZones: readonly VoronoiZoneBoundary[];
  readonly navmeshSegments: readonly NavmeshLinkSegment[];
  readonly applyWorldDelta: (delta: WorldDeltaBatch) => void;
  readonly selectEntity: (guid: string | null) => void;
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
    zones: readonly VoronoiZoneBoundary[],
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

function clampFloat64(value: number, min: number, max: number): number {
  if (!Number.isFinite(value)) {
    return 0.0;
  }
  return Math.min(max, Math.max(min, value));
}

// Float64 projection preserving sub-centimetre RTC precision across the 24km x 24km Verra extent.
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

export const DEFAULT_LAYER_TOGGLES: LayerToggles = Object.freeze({
  players: true,
  npcs: true,
  spawns: true,
  navmesh: false,
  voronoiBorders: true,
});

export const DEFAULT_CAMERA_VIEWPORT: CameraViewport = Object.freeze({
  longitude: 0.0,
  latitude: 0.0,
  zoom: 3.0,
  pitch: 0.0,
  bearing: 0.0,
  mode: "2d",
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

function buildInitialWorldStreamState(
  setState: WorldStreamStoreApi["setState"],
  getState: () => WorldStreamState,
  overrides?: Partial<WorldStreamState>,
): WorldStreamState {
  return {
    entities: new Map<string, WorldEntity>(),
    selectedEntityGuid: null,
    layers: DEFAULT_LAYER_TOGGLES,
    camera: DEFAULT_CAMERA_VIEWPORT,
    connectionState: "connecting",
    lastSeq: 0,
    tickLatencyMs: 0,
    streamHz: STREAM_TARGET_HZ,
    needsSnapshotResync: false,
    error: null,
    pickedCoordinate: null,
    voronoiZones: [],
    navmeshSegments: [],
    applyWorldDelta: (delta) => {
      const patch = computeDeltaPatch(getState(), delta);
      if (patch) {
        setState(patch);
      }
    },
    selectEntity: (guid) => setState({ selectedEntityGuid: guid }),
    toggleLayer: (layer, enabled) =>
      setState((prev) => ({
        layers: {
          ...prev.layers,
          [layer]: enabled !== undefined ? enabled : !prev.layers[layer],
        },
      })),
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
    setSpatialOverlays: (voronoiZones, navmeshSegments) =>
      setState({ voronoiZones, navmeshSegments }),
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
    const snap = defaultWorldStreamStore.getState();
    return selector ? selector(snap) : (snap as unknown as U);
  },
  {
    getState: defaultWorldStreamStore.getState,
    getInitialState: defaultWorldStreamStore.getInitialState,
    setState: defaultWorldStreamStore.setState,
    subscribe: defaultWorldStreamStore.subscribe,
  },
);
