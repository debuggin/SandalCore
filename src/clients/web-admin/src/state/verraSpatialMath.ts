// SPDX-License-Identifier: AGPL-3.0-only

export const VERRA_EXTENT_CM = 1_200_000.0;
export const VERRA_MAX_LNG_DEG = 180.0;
export const VERRA_MAX_LAT_DEG = 85.05112877980659;
export const STREAM_TARGET_HZ = 30;
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
export const DEFAULT_NODE_PLUG_SPAN_CM = 100000;
export const DEFAULT_NODE_PLUG_HALF_TILE_OFFSET_CM = 25000.0;
export const WEB_ADMIN_STORAGE_KEY = "sandalcore.web_admin.session_ui_state.v1";
export const SESSION_UI_STORAGE_KEY = "sandalcore.web_admin.session_ui_state.v1";

export interface SessionUiExtraState {
  readonly selectedEntityId: string | null;
  readonly selectedEntityPinned: boolean;
  readonly nudgeMeters: string;
  readonly autoFollowPlayer: boolean;
  readonly playersSearch: string;
  readonly teleportTargetPlayer: string;
  readonly teleportX: number;
  readonly teleportY: number;
  readonly teleportZ: number;
  readonly selectedDefId: string;
  readonly spawnSearch: string;
  readonly spawnCategory: string;
  readonly spawnLevel: number;
  readonly spawnCount: number;
  readonly spawnRadiusM: number;
  readonly collapsedSections: Readonly<Record<string, boolean>>;
  readonly logContainer: string;
  readonly logLevelFilter: string;
  readonly logStreamFilter: string;
  readonly logSearch: string;
  readonly logTailLines: number;
  readonly logAutoTail: boolean;
  readonly logAutoFollow: boolean;
  readonly autoRefreshLogs: boolean;
  readonly timeOfDayHours: number;
  readonly timeOfDaySyncedAtMs: number;
  readonly timeOfDayLabel: string;
}

export const GAME_TIME_RATE_MULTIPLIER = 9.0;

export const WORLD_TIME_OF_DAY_PRESETS: readonly { readonly hour: number; readonly label: string }[] = Object.freeze([
  { hour: 0, label: "00:00 (Midnight)" },
  { hour: 6, label: "06:00 (Dawn)" },
  { hour: 9, label: "09:00 (Morning)" },
  { hour: 12, label: "12:00 (Noon)" },
  { hour: 15, label: "15:00 (Afternoon)" },
  { hour: 18, label: "18:00 (Dusk)" },
  { hour: 21, label: "21:00 (Night)" },
]);

export function computeLiveTimeOfDayHours(anchorHours: number, syncedAtMs: number, nowMs: number = Date.now()): number {
  const base = Number.isFinite(anchorHours) ? anchorHours : 12.0;
  const sync = Number.isFinite(syncedAtMs) && syncedAtMs > 0 ? syncedAtMs : nowMs;
  const elapsedHours = (Math.max(0, nowMs - sync) / 1000) * (GAME_TIME_RATE_MULTIPLIER / 3600);
  return (((base + elapsedHours) % 24) + 24) % 24;
}

export function formatTimeOfDayLabel(hour: number): string {
  const h = (((Number.isFinite(hour) ? Number(hour) : 12) % 24) + 24) % 24;
  const totalMins = Math.floor(h * 60 + 1e-6);
  return `${String(Math.floor(totalMins / 60) % 24).padStart(2, "0")}:${String(totalMins % 60).padStart(2, "0")}`;
}

export function formatTimeOfDayClock(hours: number): string {
  const h = (((Number.isFinite(hours) ? Number(hours) : 12) % 24) + 24) % 24;
  const totalSecs = Math.floor(h * 3600 + 1e-6);
  const hh = Math.floor(totalSecs / 3600) % 24;
  const mm = Math.floor((totalSecs % 3600) / 60);
  const ss = totalSecs % 60;
  return `${String(hh).padStart(2, "0")}:${String(mm).padStart(2, "0")}:${String(ss).padStart(2, "0")}`;
}

export function formatTimePhaseLabel(hours: number): string {
  const h = (((Number.isFinite(hours) ? Number(hours) : 12) % 24) + 24) % 24;
  if (h >= 5 && h < 8) return "Dawn";
  if (h >= 8 && h < 12) return "Morning";
  if (h >= 12 && h < 14) return "Noon";
  if (h >= 14 && h < 17) return "Afternoon";
  if (h >= 17 && h < 20) return "Dusk";
  return "Night";
}

export function parseTimeOfDayInput(raw: string | number): number {
  if (typeof raw === "number") return (((Number.isFinite(raw) ? raw : 12) % 24) + 24) % 24;
  const trimmed = String(raw ?? "").trim();
  if (trimmed.includes(":")) {
    const [hhS, mmS, ssS] = trimmed.split(":");
    const hh = Number(hhS) || 0, mm = Number(mmS) || 0, ss = Number(ssS) || 0;
    return (((hh + mm / 60 + ss / 3600) % 24) + 24) % 24;
  }
  const num = Number(trimmed);
  return (((Number.isFinite(num) ? num : 12) % 24) + 24) % 24;
}

export const DEFAULT_SESSION_UI_EXTRA_STATE: SessionUiExtraState = Object.freeze({
  selectedEntityId: null,
  selectedEntityPinned: false,
  nudgeMeters: "8",
  autoFollowPlayer: false,
  playersSearch: "",
  teleportTargetPlayer: "",
  teleportX: -644519,
  teleportY: 376773,
  teleportZ: 12012,
  selectedDefId: "Aela",
  spawnSearch: "",
  spawnCategory: "ALL",
  spawnLevel: 15,
  spawnCount: 1,
  spawnRadiusM: 5,
  collapsedSections: Object.freeze({}),
  logContainer: "aoc-world-riverlands",
  logLevelFilter: "ALL",
  logStreamFilter: "ALL",
  logSearch: "",
  logTailLines: 100,
  logAutoTail: true,
  logAutoFollow: true,
  autoRefreshLogs: true,
  timeOfDayHours: 12.0,
  timeOfDaySyncedAtMs: Date.now(),
  timeOfDayLabel: "12:00",
});

const fallbackTabSessionMap = new Map<string, string>();
const fallbackTabSessionStorage = {
  getItem: (k: string): string | null => fallbackTabSessionMap.get(k) ?? null,
  setItem: (k: string, v: string): void => { fallbackTabSessionMap.set(k, String(v)); },
  removeItem: (k: string): void => { fallbackTabSessionMap.delete(k); },
  clear: (): void => { fallbackTabSessionMap.clear(); },
};
if (typeof globalThis !== "undefined" && typeof (globalThis as { sessionStorage?: unknown }).sessionStorage === "undefined") {
  (globalThis as { sessionStorage?: typeof fallbackTabSessionStorage }).sessionStorage = fallbackTabSessionStorage;
}

export function resolveTabSessionStorage<T extends Pick<Storage, "getItem"> | Pick<Storage, "setItem">>(explicitStorage?: T | null): T | null {
  if (explicitStorage !== undefined) return explicitStorage;
  if (typeof window !== "undefined" && window.sessionStorage) return window.sessionStorage as unknown as T;
  if (typeof globalThis !== "undefined" && (globalThis as { sessionStorage?: unknown }).sessionStorage) {
    return (globalThis as unknown as { sessionStorage: T }).sessionStorage;
  }
  return fallbackTabSessionStorage as unknown as T;
}

export function clearTabSessionStorage(): void {
  fallbackTabSessionMap.clear();
  try {
    if (typeof window !== "undefined" && window.sessionStorage) window.sessionStorage.removeItem(SESSION_UI_STORAGE_KEY);
  } catch { /* ignore */ }
}

export const NODE_STAGE_NAMES: readonly string[] = Object.freeze([
  "Wilderness",
  "Expedition",
  "Encampment",
  "Village",
  "Town",
  "City",
  "Metropolis",
]);

const NODE_PLUG_TIER_BY_LEVEL: readonly number[] = Object.freeze([0, 0, 1, 2, 3, 4, 4]);

export type LngLatTuple = readonly [number, number];
export type MosaicPxTuple = readonly [number, number];
export type WorldCmTuple = readonly [number, number];

export interface VerraUnrealPoint {
  readonly xCm: number;
  readonly yCm: number;
  readonly zCm: number;
}

export interface PoiOverlay {
  readonly guid: string;
  readonly name: string;
  readonly display_name: string;
  readonly category: string;
  readonly x_cm: number;
  readonly y_cm: number;
  readonly z_cm: number;
  readonly icon_name: string;
  readonly icon_url: string;
  readonly tint_color?: string;
  readonly icon_size?: number;
}

export interface VerraCityNodeMeta {
  readonly node_index: number;
  readonly guid: string;
  readonly name: string;
  readonly display_name: string;
  readonly ox: number;
  readonly oy: number;
  readonly oz?: number;
  readonly x_cm?: number;
  readonly y_cm?: number;
  readonly z_cm?: number;
  readonly offset_x_cm?: number;
  readonly offset_y_cm?: number;
  readonly plug_scale?: number;
  readonly plug_size_cm?: number;
  readonly plug_rotation_deg?: number;
  readonly yaw_deg: number;
  readonly size_cm: number;
  readonly level: number;
  readonly plug_tier: number;
  readonly mayor_guid?: string;
  readonly mayor_name?: string;
  readonly election_status?: string;
}

export type CityNodeOverlay = VerraCityNodeMeta;

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

export interface BaseTileDescriptor {
  readonly z: number;
  readonly x: number;
  readonly y: number;
  readonly tileUrl: string;
  readonly mosaicU0: number;
  readonly mosaicV0: number;
  readonly mosaicSpanPx: number;
  readonly distFromCenterPx?: number;
}

export interface NodePlugTileOverlayDescriptor {
  readonly nodeIndex: number;
  readonly name: string;
  readonly displayName: string;
  readonly level: number;
  readonly plugTier: number;
  readonly tileUrl: string;
  readonly cxCm: number;
  readonly cyCm: number;
  readonly centerMosaicU: number;
  readonly centerMosaicV: number;
  readonly sizeMosaicPx: number;
  readonly sizeCm: number;
  readonly worldSpanCm: number;
  readonly drawSpanPx: number;
  readonly yawDeg: number;
  readonly rotationRad: number;
}

export function worldCmToMosaicPx(xCm: number, yCm: number): MosaicPxTuple {
  return [
    ((xCm + DEFAULT_NODE_PLUG_HALF_TILE_OFFSET_CM) / VERRA_MAP_SOURCE_CM - VERRA_MAP_X0) * VERRA_MAP_SOURCE_PX,
    ((yCm + DEFAULT_NODE_PLUG_HALF_TILE_OFFSET_CM) / VERRA_MAP_SOURCE_CM - VERRA_MAP_Y0) * VERRA_MAP_SOURCE_PX,
  ];
}

export function mosaicPxToWorldCm(u: number, v: number): WorldCmTuple {
  return [
    (u / VERRA_MAP_SOURCE_PX + VERRA_MAP_X0) * VERRA_MAP_SOURCE_CM - DEFAULT_NODE_PLUG_HALF_TILE_OFFSET_CM,
    (v / VERRA_MAP_SOURCE_PX + VERRA_MAP_Y0) * VERRA_MAP_SOURCE_CM - DEFAULT_NODE_PLUG_HALF_TILE_OFFSET_CM,
  ];
}

export function getCityNodeCenterCm(node: {
  readonly x_cm?: number;
  readonly y_cm?: number;
  readonly z_cm?: number;
  readonly ox?: number;
  readonly oy?: number;
  readonly oz?: number;
  readonly offset_x_cm?: number;
  readonly offset_y_cm?: number;
}): { readonly cx_cm: number; readonly cy_cm: number; readonly xCm: number; readonly yCm: number; readonly zCm: number } {
  const ox = Number.isFinite(node.offset_x_cm) ? Number(node.offset_x_cm) : 0;
  const oy = Number.isFinite(node.offset_y_cm) ? Number(node.offset_y_cm) : 0;
  const baseX = Number.isFinite(node.x_cm) ? Number(node.x_cm) : Number(node.ox || 0);
  const baseY = Number.isFinite(node.y_cm) ? Number(node.y_cm) : Number(node.oy || 0);
  const baseZ = Number.isFinite(node.z_cm) ? Number(node.z_cm) : Number(node.oz || 0);
  const cx_cm = baseX + ox, cy_cm = baseY + oy;
  return { cx_cm, cy_cm, xCm: cx_cm, yCm: cy_cm, zCm: baseZ };
}

export function nodeLevelToPlugTier(level: number): number {
  return Number.isFinite(level)
    ? (NODE_PLUG_TIER_BY_LEVEL[Math.max(0, Math.min(6, Math.trunc(level)))] ?? 0)
    : 0;
}

export function computeNodePlugTileOverlay(
  node: VerraCityNodeMeta,
  cameraZoom = 5.0,
): NodePlugTileOverlayDescriptor | null {
  const plugTier = nodeLevelToPlugTier(node.level);
  if (plugTier < 0) return null;
  const { cx_cm: cxCm, cy_cm: cyCm } = getCityNodeCenterCm(node);
  const [u, v] = worldCmToMosaicPx(cxCm, cyCm);
  const scale = Math.pow(2, cameraZoom - 5.0);
  const worldSpanCm = (node.plug_size_cm ?? DEFAULT_NODE_PLUG_SPAN_CM) * (node.plug_scale ?? 1.0);
  const drawSpanPx = ((worldSpanCm * VERRA_NODE_PLUG_SIZE_PX) / VERRA_NODE_PLUG_SIZE_CM) * scale;
  const rotDeg = node.plug_rotation_deg !== undefined ? node.plug_rotation_deg : node.yaw_deg;
  const sizeMosaicPx =
    node.plug_size_cm !== undefined || node.plug_scale !== undefined
      ? (worldSpanCm * VERRA_NODE_PLUG_SIZE_PX) / VERRA_NODE_PLUG_SIZE_CM
      : VERRA_NODE_PLUG_SIZE_PX;
  return {
    nodeIndex: node.node_index,
    name: node.name,
    displayName: node.display_name,
    level: node.level,
    plugTier,
    tileUrl: `/api/v1/world/tiles/${VERRA_MAP_PLUG_ZOOM}/${node.node_index}/${plugTier}`,
    cxCm,
    cyCm,
    centerMosaicU: u,
    centerMosaicV: v,
    sizeMosaicPx,
    sizeCm: node.size_cm || VERRA_NODE_PLUG_SIZE_CM,
    worldSpanCm,
    drawSpanPx,
    yawDeg: node.yaw_deg,
    rotationRad: (rotDeg * Math.PI) / 180.0,
  };
}

export function computeVisibleBaseTiles(
  meta: VerraMapMeta,
  centerXCm: number,
  centerYCm: number,
  zoom: number,
  viewportSize: { readonly width: number; readonly height: number },
): readonly BaseTileDescriptor[] {
  const scale = Math.pow(2, zoom - 5.0);
  const rawZ = Math.ceil(Math.log2(Math.max(1e-6, scale * (meta.width_px / meta.tile_px)))) + 1;
  const z = Math.max(0, Math.min(meta.max_zoom, rawZ));
  const spanPx = meta.tile_px * Math.pow(2, meta.max_zoom - z);
  const [centerU, centerV] = worldCmToMosaicPx(centerXCm, centerYCm);
  const halfSpanU = Math.max(spanPx * 0.5, viewportSize.width / Math.max(1e-4, scale) / 2);
  const halfSpanV = Math.max(spanPx * 0.5, viewportSize.height / Math.max(1e-4, scale) / 2);
  const tx0 = Math.max(0, Math.floor((centerU - halfSpanU) / spanPx));
  const tx1 = Math.min(Math.max(0, Math.ceil(meta.width_px / spanPx) - 1), Math.floor((centerU + halfSpanU) / spanPx));
  const ty0 = Math.max(0, Math.floor((centerV - halfSpanV) / spanPx));
  const ty1 = Math.min(Math.max(0, Math.ceil(meta.height_px / spanPx) - 1), Math.floor((centerV + halfSpanV) / spanPx));
  const out: BaseTileDescriptor[] = [];
  for (let ty = ty0; ty <= ty1; ty += 1) {
    for (let tx = tx0; tx <= tx1; tx += 1) {
      out.push({
        z,
        x: tx,
        y: ty,
        tileUrl: `/api/v1/world/tiles/${z}/${tx}/${ty}`,
        mosaicU0: tx * spanPx,
        mosaicV0: ty * spanPx,
        mosaicSpanPx: spanPx,
        distFromCenterPx: Math.hypot((tx + 0.5) * spanPx - centerU, (ty + 0.5) * spanPx - centerV),
      });
    }
  }
  return out.sort((a, b) => (a.distFromCenterPx ?? 0) - (b.distFromCenterPx ?? 0));
}

function clampFloat64(value: number, min: number, max: number): number {
  return Number.isFinite(value) ? Math.min(max, Math.max(min, value)) : 0.0;
}

export function verraUnrealToLngLat(xCm: number, yCm: number): LngLatTuple {
  return [
    (clampFloat64(xCm, -VERRA_EXTENT_CM, VERRA_EXTENT_CM) / VERRA_EXTENT_CM) * VERRA_MAX_LNG_DEG,
    (clampFloat64(yCm, -VERRA_EXTENT_CM, VERRA_EXTENT_CM) / VERRA_EXTENT_CM) * VERRA_MAX_LAT_DEG,
  ];
}

export function lngLatToVerraUnreal(lng: number, lat: number, zCm = 0.0): VerraUnrealPoint {
  return {
    xCm: (clampFloat64(lng, -VERRA_MAX_LNG_DEG, VERRA_MAX_LNG_DEG) / VERRA_MAX_LNG_DEG) * VERRA_EXTENT_CM,
    yCm: (clampFloat64(lat, -VERRA_MAX_LAT_DEG, VERRA_MAX_LAT_DEG) / VERRA_MAX_LAT_DEG) * VERRA_EXTENT_CM,
    zCm: Number.isFinite(zCm) ? zCm : 0.0,
  };
}

function parseCityNodeSpec(spec: string, node_index: number): VerraCityNodeMeta {
  const [guid, suffix, display_name, oxS, oyS, yawS, lvlS, ozS] = spec.split("|");
  const ox = Number(oxS);
  const oy = Number(oyS);
  const oz = Number(ozS ?? 0);
  const yaw_deg = Number(yawS);
  const level = Number(lvlS);
  return {
    node_index,
    guid,
    name: `Verra_${suffix}`,
    display_name,
    ox,
    oy,
    oz,
    x_cm: ox,
    y_cm: oy,
    z_cm: oz,
    yaw_deg,
    size_cm: VERRA_NODE_PLUG_SIZE_CM,
    plug_size_cm: DEFAULT_NODE_PLUG_SPAN_CM,
    plug_scale: 1.0,
    level,
    plug_tier: nodeLevelToPlugTier(level),
    mayor_name: "",
    election_status: "none",
  };
}

export const DEFAULT_VERRA_CITY_NODES: readonly VerraCityNodeMeta[] = Object.freeze(
  [
    "108646108518008|RVR_Winstead|Winstead|-644519.11|376773.24|319.20846|3|12012.23",
    "108646121231993|RVR_Joeva|Joeva|-1094396.04|521336.34|292.625506|3|11836.45",
    "108646608033403|RVR_NewAela|New Aela|-1134183.54|853387.28|-90.21496|3|0",
    "108646610851452|RVR_Halcyon|Halcyon|-891024.5|296919.66|180.0|3|11657.05",
    "108646615701117|RVR_Miraleth|Miraleth|-459889.63|599317.09|-20.0|3|0",
    "6064632225073725498|TUR_Windansea|Windansea|-1100624.18|246466.99|55.41214|0|0",
    "6064632734251679746|SAN_Aithanahr|Aithanahr|-232617.63|762558.56|304.121675|0|6679.26",
    "6064632734253907971|SAN_Azmaran|Azmaran|-278767.94|987391.03|351.156897|0|7227.0",
    "6064632734268915716|SAN_Sunhaven|Sunhaven|-619803.28|873514.22|-127.073107|0|0",
    "6064633097318170624|SAN_Djinna|Djinna|63370.91|861198.37|-187.137862|0|0",
    "6064633097371058177|SAN_SquallsEnd|Squall's End|-37370.0|1133335.0|-217.228668|0|0",
    "6064633334652600320|TUR_Korrin|Korrin|-1764281.28|114557.65|90.707388|0|0",
    "6064633334741794817|TUR_Brinebarrel|Brinebarrel|-1860980.22|-171832.95|-46.566062|0|0",
    "6064633407845105664|TUR_Seahook|Seahook|-1764263.89|539283.28|14.228906|0|0",
    "6064633407846088705|TUR_Shorefoot|Shorefoot|-1372642.27|57374.67|-66.900158|0|0",
    "6064633425419042816|JUN_Wildport|Wildport|-1783982.0|809682.0|16.0|0|9777.0",
    "6064633425517019141|JUN_Hecribba|Hecribba|-1708500.0|1053019.0|246.0|0|9620.0",
    "6064633425555816457|JUN_Vinebreach|Vinebreach|-1476480.0|1178456.0|200.0|0|0",
    "6064633429404090381|JUN_Arisalon|Arisalon|-1250506.0|1062377.0|-35.0|0|0",
    "6064633429423882257|JUN_TangledPost|Tangled Post|-1031419.0|1098848.0|-3.5|0|0",
    "6064633429437251605|JUN_Mythbreak|Mythbreak|-742705.0|1087738.0|-159.058146|0|0",
    "6064633651969589265|ANV_Vexhelm|Vexhelm|-1463243.0|-341306.0|60.0|0|25074.0",
    "6064633651985711129|ANV_Duunhold|Duunhold|-1069784.0|-150970.0|0.0|0|0",
    "6064633652009173023|ANV_Dhurgrum|Dhurgrum|-937374.0|-411205.0|-15.0|0|0",
    "6064633652023394340|ANV_Vhalgadim|Vhalgadim|-762862.0|-563881.0|349.999997|0|17256.0",
    "6064633652044300331|ANV_KalTorhum|Kal Torhum|-1271772.0|-462821.0|5.0|0|5066.0",
  ].map(parseCityNodeSpec),
);

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

const SUBPARTITION_TICK_OVERRIDES = new Map<string, number>();

export function setSubpartitionWorkerTickHz(workerId: string, tickHz: number): void {
  SUBPARTITION_TICK_OVERRIDES.set(workerId, tickHz);
}

function parseSubpartitionWorkerSpec(spec: string, idx: number) {
  const [worker_id, canonical_name, scope_key, scope_kind, region_label, container_name, hz] = spec.split("|");
  return Object.freeze({
    worker_id, worker_index: 10 + idx, canonical_name, scope_key, scope_kind, region_label, container_name,
    dynamic_scaling_stub: false, status: "UP", npc_count: 18, monster_count: 6, resource_node_count: 12,
    player_count: 0, npcs: 18, monsters: 6, resource_nodes: 12, active_players: 0,
    tick_hz: Number(hz), tick_p50_ms: 0.4, tick_p99_ms: 1.6,
  });
}

export const DEFAULT_SUBPARTITION_WORKERS = Object.freeze([
  "world-node-winstead|aoc-winstead|Verra_RVR_Winstead|node_plug|Riverlands · Winstead Node|aoc-world-riverlands|100",
  "world-node-joeva|aoc-joeva|Verra_RVR_Joeva|node_plug|Riverlands · Joeva Node|aoc-world-riverlands|100",
  "world-node-halcyon|aoc-halcyon|Verra_RVR_Halcyon|node_plug|Riverlands · Halcyon Node|aoc-world-riverlands|100",
  "world-node-miraleth|aoc-miraleth|Verra_RVR_Miraleth|node_plug|Riverlands · Miraleth Node|aoc-world-riverlands|100",
  "world-node-new-aela|aoc-new-aela|Verra_RVR_NewAela|node_plug|Riverlands · New Aela Node|aoc-world-riverlands|100",
  "world-zoi-winstead|aoc-riverlands-node-winstead|Verra_RVR_Node_Winstead|zoi|Riverlands · Winstead ZOI|aoc-world-riverlands|60",
  "world-zoi-joeva|aoc-riverlands-node-joeva|Verra_RVR_Node_Joeva|zoi|Riverlands · Joeva ZOI|aoc-world-riverlands|60",
  "world-zoi-halcyon|aoc-riverlands-node-halcyon|Verra_RVR_Node_Halcyon|zoi|Riverlands · Halcyon ZOI|aoc-world-riverlands|60",
  "world-zoi-miraleth|aoc-riverlands-node-miraleth|Verra_RVR_Node_Miraleth|zoi|Riverlands · Miraleth ZOI|aoc-world-riverlands|60",
  "world-zoi-sunhaven|aoc-sandsquall-node-sunhaven|Verra_SAN_Node_Sunhaven|zoi|Sandsquall · Sunhaven ZOI|aoc-world-sandsquall|60",
  "world-zoi-djinna|aoc-sandsquall-node-djinna|Verra_SAN_Node_Djinna|zoi|Sandsquall · Djinna ZOI|aoc-world-sandsquall|60",
  "world-zoi-azmaran|aoc-sandsquall-node-azmaran|Verra_SAN_Node_Azmaran|zoi|Sandsquall · Azmaran ZOI|aoc-world-sandsquall|60",
  "world-zoi-aithanahr|aoc-sandsquall-node-aithanahr|Verra_SAN_Node_Aithanahr|zoi|Sandsquall · Aithanahr ZOI|aoc-world-sandsquall|60",
  "world-zoi-squallsend|aoc-sandsquall-node-squallsend|Verra_SAN_Node_SquallsEnd|zoi|Sandsquall · Squall's End ZOI|aoc-world-sandsquall|60",
  "world-node-sunhaven|aoc-sunhaven|Verra_SAN_Sunhaven|node_plug|Sandsquall · Sunhaven Node|aoc-world-sandsquall|100",
  "world-node-azmaran|aoc-azmaran|Verra_SAN_Azmaran|node_plug|Sandsquall · Azmaran Node|aoc-world-sandsquall|100",
].map(parseSubpartitionWorkerSpec));

export function expandViewportWorkerCatalog<T extends { readonly worker_id: string; readonly canonical_name?: string; readonly scope_key: string; readonly scope_kind: string; readonly tick_hz?: number }>(
  baseWorkers: readonly T[],
): readonly T[] {
  const seen = new Set(baseWorkers.map((w) => `${w.scope_kind}:${w.scope_key}`));
  const extras = (DEFAULT_SUBPARTITION_WORKERS as unknown as readonly T[])
    .filter((w) => !seen.has(`${w.scope_kind}:${w.scope_key}`))
    .map((w) => {
      const hz = SUBPARTITION_TICK_OVERRIDES.get(w.worker_id) ?? (w.canonical_name ? SUBPARTITION_TICK_OVERRIDES.get(w.canonical_name) : undefined);
      return hz !== undefined ? { ...w, tick_hz: hz } : w;
    });
  return [...baseWorkers, ...extras];
}

