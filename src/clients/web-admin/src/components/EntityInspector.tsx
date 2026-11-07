// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import {
  findZoiContainingWorldCm,
  inferResourceNodeType,
  type PoiOverlay,
  resolveWorkerForCoordinateOrZone,
  useWorldStreamStore,
  verraUnrealToLngLat,
  worldCmToMosaicPx,
  type WorldEntity,
  type WorldStreamState,
  type WorldWorkerInfo,
} from "../state/worldStreamStore";

export type InspectorViewState =
  | "loading"
  | "empty"
  | "error"
  | "degraded"
  | "populated";

export interface InspectorGridRow {
  readonly rowId: string;
  readonly label: string;
  readonly value: string;
}

export interface InspectedWorldEntity extends WorldEntity {
  readonly x?: number;
  readonly y?: number;
  readonly z?: number;
  readonly hp?: number;
  readonly maxHp?: number;
  readonly stateFlags?: readonly string[];
  readonly activeBuffs?: readonly string[];
}

export interface EntityInspectorProps {
  readonly entity: InspectedWorldEntity | null;
  readonly viewState?: InspectorViewState;
  readonly activeRowIndex?: number;
  readonly errorDetail?: string;
  readonly showQuickActions?: boolean;
  readonly onClose?: () => void;
}

export interface ExclusiveInspectorResolution {
  readonly activeKind: "none" | "entity" | "poi" | "city_node" | "position";
  readonly entity: InspectedWorldEntity | null;
  readonly cityNodeName: string | null;
  readonly pickedPosition: { readonly xCm: number; readonly yCm: number; readonly zCm: number } | null;
}

export interface EntityInspectorModel {
  readonly role: "grid";
  readonly viewState: InspectorViewState;
  readonly activeRowIndex: number;
  readonly contrastRatioMin: number;
  readonly rows: readonly InspectorGridRow[];
  readonly statusMessage: string | null;
}

export const INSPECTOR_CONTRAST_RATIO_AA = 15.8;

function formatSafeValue(raw: unknown): string {
  if (raw === null || raw === undefined) {
    return "N/A";
  }
  if (typeof raw === "number") {
    return Number.isFinite(raw) ? String(raw) : "N/A";
  }
  if (typeof raw === "boolean" || typeof raw === "bigint") {
    return String(raw);
  }
  if (typeof raw === "string") {
    return raw.trim().length > 0 ? raw : "N/A";
  }
  return JSON.stringify(raw);
}

export function moveInspectorGridRowFocus(
  currentIndex: number,
  rowCount: number,
  key: string,
): number {
  if (rowCount <= 0) {
    return 0;
  }
  const clamped = Math.max(0, Math.min(rowCount - 1, Math.trunc(currentIndex)));
  if (key === "ArrowDown") return Math.min(rowCount - 1, clamped + 1);
  if (key === "ArrowUp") return Math.max(0, clamped - 1);
  if (key === "Home") return 0;
  if (key === "End") return rowCount - 1;
  return clamped;
}

function resolveStateFlagsList(entity: InspectedWorldEntity): string {
  if (entity.stateFlags && entity.stateFlags.length > 0) {
    return entity.stateFlags.join(", ");
  }
  const derived: string[] = [];
  if (entity.dead) derived.push("DEAD");
  if (entity.inCombat) derived.push("IN_COMBAT");
  if (entity.mounted) derived.push("MOUNTED");
  return derived.length > 0 ? derived.join(", ") : "IDLE";
}

export function formatPoiCategoryDisplay(poi: {
  readonly category?: string; readonly kind?: string; readonly icon_name?: string;
  readonly name?: string; readonly display_name?: string; readonly npc_subkind?: string; readonly npcSubkind?: string;
}): string {
  const s = `${poi.category ?? ""} ${poi.kind ?? ""} ${poi.icon_name ?? ""} ${poi.npcSubkind ?? poi.npc_subkind ?? ""} ${poi.name ?? ""} ${poi.display_name ?? ""}`.toLowerCase();
  if (/wayshrine|emberspring|ember_spring|ember spring|attune|reviveshrine|shrine/.test(s)) return "Ember Spring";
  if (/dungeon/.test(s)) return "Dungeon";
  if (/landmark/.test(s)) return "Landmark";
  if (/waypoint/.test(s)) return "Waypoint";
  if (/harbor/.test(s)) return "Harbor";
  if (/castle/.test(s)) return "Castle";
  if (/outpost/.test(s)) return "Outpost";
  if (/starting_area|startingarea|portal/.test(s)) return "Starting Area";
  if (/spawn/.test(s)) return "Spawn Point";
  if (/city_node/.test(s)) return "Settlement Node";
  if (/zone/.test(s)) return "Zone";
  const raw = (poi.category ?? poi.kind ?? "").trim();
  return raw && raw.toLowerCase() !== "poi" && raw.toLowerCase() !== "npc" ? `${raw.charAt(0).toUpperCase()}${raw.slice(1)}` : "POI";
}

export function poiOverlayToInspectedEntity(
  poi: PoiOverlay,
  workers: readonly WorldWorkerInfo[] = useWorldStreamStore.getState().workers,
): InspectedWorldEntity {
  const zoi = findZoiContainingWorldCm(poi.x_cm, poi.y_cm);
  const worker = resolveWorkerForCoordinateOrZone(poi.x_cm, poi.y_cm, poi.biome, workers, poi.node_name || poi.display_name);
  const regionZoi = zoi ? `${zoi.display_name} · ${worker.region_label}` : worker.region_label;
  const nodeParts = [poi.node_name, poi.node_type, poi.stage_name, poi.biome].filter((p): p is string => Boolean(p && p.trim().length > 0));
  const uniqueNodeParts = [...new Set(nodeParts)];
  const levelRange = poi.min_level !== undefined && poi.max_level !== undefined ? `${poi.min_level}-${poi.max_level}` : poi.level && poi.level > 0 ? String(poi.level) : undefined;
  const descParts = [poi.description, poi.tags?.join(", ")].filter((p): p is string => Boolean(p && p.trim().length > 0));
  return {
    guid: poi.guid, name: poi.display_name || poi.name, recordName: poi.name, kind: "poi",
    poiCategory: formatPoiCategoryDisplay(poi), regionZoi,
    xCm: poi.x_cm, yCm: poi.y_cm, zCm: poi.z_cm, yawDeg: poi.yaw ?? 0,
    level: poi.level ?? 1, healthPct: 100, radiusCm: poi.radius,
    nodeAssociation: uniqueNodeParts.length > 0 ? uniqueNodeParts.join(" · ") : undefined,
    levelRange, description: descParts.length > 0 ? descParts.join(" | ") : undefined,
  };
}

function buildPoiSpecificRows(entity: InspectedWorldEntity, zoiName: string | undefined, regionLabel: string): readonly InspectorGridRow[] {
  const isPoiOrShrine = entity.kind === "poi" || (entity.npcSubkind ?? entity.npc_subkind) === "emberspring" || Boolean(entity.poiCategory);
  if (!isPoiOrShrine) return [];
  const rows: InspectorGridRow[] = [
    { rowId: "category", label: "Category / Subtype", value: entity.poiCategory || formatPoiCategoryDisplay(entity) },
    { rowId: "record_name", label: "Record Name", value: formatSafeValue(entity.recordName ?? entity.name) },
    { rowId: "region_zoi", label: "Region / ZOI", value: entity.regionZoi || (zoiName ? `${zoiName} · ${regionLabel}` : regionLabel) },
  ];
  if (entity.nodeAssociation) rows.push({ rowId: "node_association", label: "Node / Settlement", value: entity.nodeAssociation });
  if (typeof entity.radiusCm === "number" && entity.radiusCm > 0) rows.push({ rowId: "radius", label: "Radius", value: `${(entity.radiusCm / 100).toFixed(1)} m (${Math.round(entity.radiusCm)} cm)` });
  if (entity.description) rows.push({ rowId: "description", label: "Description / Tags", value: entity.description });
  return rows;
}

function buildEntityAttributeRows(entity: InspectedWorldEntity | null): readonly InspectorGridRow[] {
  if (!entity) return [];
  const [lng, lat] = verraUnrealToLngLat(entity.xCm, entity.yCm), [mosaicU, mosaicV] = worldCmToMosaicPx(entity.xCm, entity.yCm);
  const pct = Number.isFinite(entity.healthPct) ? entity.healthPct.toFixed(1) : "0.0";
  const hasHp = typeof entity.hp === "number" && typeof entity.maxHp === "number" && Number.isFinite(entity.hp) && Number.isFinite(entity.maxHp);
  const hpText = hasHp ? `${Math.round(entity.hp!)} / ${Math.round(entity.maxHp!)} (${pct}%)` : `${pct}%`;
  const buffs = entity.activeBuffs && entity.activeBuffs.length > 0 ? entity.activeBuffs.join(", ") : "None";
  const worker = resolveWorkerForCoordinateOrZone(entity.xCm, entity.yCm, entity.zoneId, useWorldStreamStore.getState().workers, entity.name);
  const zoi = findZoiContainingWorldCm(entity.xCm, entity.yCm);
  const rawResType = entity.resourceType ?? entity.resource_type, rawSubkind = entity.npcSubkind ?? entity.npc_subkind;
  const resourceRows: InspectorGridRow[] = entity.kind === "resource_node" || Boolean(rawResType) || rawSubkind === "huntable"
    ? [
        { rowId: "resource_type", label: "Resource Type", value: inferResourceNodeType(entity.name, rawResType, rawSubkind) },
        { rowId: "resource_quality", label: "Quality / Rarity", value: entity.quality || "Common · Tier 1 (Novice)" },
      ]
    : [];
  const poiRows = buildPoiSpecificRows(entity, zoi?.display_name, worker.region_label);
  return [
    { rowId: "guid", label: "GUID", value: formatSafeValue(entity.guid) },
    { rowId: "name", label: "Name", value: formatSafeValue(entity.name) },
    { rowId: "kind", label: "Kind", value: formatSafeValue(entity.kind) },
    ...poiRows,
    ...resourceRows,
    { rowId: "worker", label: "Server Worker", value: `${worker.worker_id} · ${worker.region_label}` },
    { rowId: "level", label: "Level", value: entity.levelRange ?? formatSafeValue(entity.level) },
    { rowId: "hp", label: "HP / MaxHP", value: hpText },
    { rowId: "unreal_cm", label: "Unreal (cm)", value: `X=${entity.xCm.toFixed(1)}, Y=${entity.yCm.toFixed(1)}, Z=${entity.zCm.toFixed(1)}` },
    { rowId: "mosaic_px", label: "Mosaic (px)", value: `U=${mosaicU.toFixed(1)}, V=${mosaicV.toFixed(1)}` },
    { rowId: "lng_lat", label: "WGS84 (lng, lat)", value: `${lng.toFixed(5)}, ${lat.toFixed(5)}` },
    { rowId: "state_flags", label: "State / Buffs", value: `${resolveStateFlagsList(entity)} (${buffs})` },
  ];
}

function resolveStatusMessage(
  viewState: InspectorViewState,
  errorDetail?: string,
): string | null {
  if (viewState === "loading") return "Loading entity telemetry...";
  if (viewState === "empty")
    return "No entity selected. Click an entity on the Verra map.";
  if (viewState === "error")
    return errorDetail ?? "Entity inspection stream failed.";
  if (viewState === "degraded")
    return "Stale telemetry: stream sequence gap detected.";
  return null;
}

export function buildEntityInspectorModel(
  props: EntityInspectorProps,
): EntityInspectorModel {
  const rows = buildEntityAttributeRows(props.entity);
  const viewState: InspectorViewState =
    props.viewState ?? (props.entity ? "populated" : "empty");
  const activeRowIndex = Math.max(
    0,
    Math.min(Math.max(0, rows.length - 1), props.activeRowIndex ?? 0),
  );
  return {
    role: "grid",
    viewState,
    activeRowIndex,
    contrastRatioMin: INSPECTOR_CONTRAST_RATIO_AA,
    rows,
    statusMessage: resolveStatusMessage(viewState, props.errorDetail),
  };
}

export function resolveMutuallyExclusiveInspectorTarget(
  storeSnap: Pick<
    WorldStreamState,
    | "entities"
    | "pois"
    | "selectedEntityGuid"
    | "selectedPoiGuid"
    | "selectedCityNodeName"
    | "pickedCoordinate"
    | "lastClickedPoint"
  >,
): ExclusiveInspectorResolution {
  const pin = storeSnap.pickedCoordinate ?? storeSnap.lastClickedPoint;
  if (pin) {
    return {
      activeKind: "position",
      entity: null,
      cityNodeName: null,
      pickedPosition: { xCm: pin.xCm, yCm: pin.yCm, zCm: pin.zCm },
    };
  }
  if (storeSnap.selectedCityNodeName) {
    return {
      activeKind: "city_node",
      entity: null,
      cityNodeName: storeSnap.selectedCityNodeName,
      pickedPosition: null,
    };
  }
  if (storeSnap.selectedPoiGuid) {
    const poi = storeSnap.pois.find((p) => p.guid === storeSnap.selectedPoiGuid);
    return {
      activeKind: poi ? "poi" : "none",
      entity: poi ? poiOverlayToInspectedEntity(poi) : null,
      cityNodeName: null,
      pickedPosition: null,
    };
  }
  if (storeSnap.selectedEntityGuid) {
    const ent = storeSnap.entities.get(storeSnap.selectedEntityGuid) ?? null;
    return {
      activeKind: ent ? "entity" : "none",
      entity: ent,
      cityNodeName: null,
      pickedPosition: null,
    };
  }
  return { activeKind: "none", entity: null, cityNodeName: null, pickedPosition: null };
}

export function openTeleportModalFromEntity(
  entity: InspectedWorldEntity,
  storeSnap: WorldStreamState = useWorldStreamStore.getState(),
): void {
  const xCm = Math.round(Number(entity.x ?? entity.xCm ?? 0));
  const yCm = Math.round(Number(entity.y ?? entity.yCm ?? 0));
  const zCm = Math.round(Number(entity.z ?? entity.zCm ?? 0));
  storeSnap.setPickedCoordinate({ xCm, yCm, zCm });
  storeSnap.setTeleportMode("coords");
  storeSnap.setActiveModal("teleport");
}

export function openSpawnNpcModalFromEntity(
  entity: InspectedWorldEntity,
  storeSnap: WorldStreamState = useWorldStreamStore.getState(),
): void {
  const xCm = Math.round(Number(entity.x ?? entity.xCm ?? 0));
  const yCm = Math.round(Number(entity.y ?? entity.yCm ?? 0));
  const zCm = Math.round(Number(entity.z ?? entity.zCm ?? 0));
  storeSnap.setPickedCoordinate({ xCm, yCm, zCm });
  storeSnap.setSpawnTargetMode("coords");
  storeSnap.setActiveModal("spawn-npc");
}

function renderEntityInspectorActionButtons(
  entity: InspectedWorldEntity,
  storeSnap: WorldStreamState,
): React.ReactElement {
  return (
    <div style={{ display: "flex", gap: "6px", flexWrap: "wrap", marginTop: "8px" }}>
      <button
        type="button"
        data-testid="inspector-teleport-here-btn"
        onClick={() => openTeleportModalFromEntity(entity, storeSnap)}
        style={{
          minHeight: "32px",
          padding: "4px 8px",
          borderRadius: "4px",
          border: "1px solid rgba(140, 108, 66, 0.67)",
          backgroundColor: "#E8B25A",
          color: "#0B0E17",
          fontWeight: 700,
          fontSize: "11px",
          cursor: "pointer",
        }}
      >
        Teleport Player Here...
      </button>
      <button
        type="button"
        data-testid="inspector-spawn-next-btn"
        onClick={() => openSpawnNpcModalFromEntity(entity, storeSnap)}
        style={{
          minHeight: "32px",
          padding: "4px 8px",
          borderRadius: "4px",
          border: "1px solid rgba(140, 108, 66, 0.47)",
          backgroundColor: "rgba(22, 30, 46, 0.92)",
          color: "#F4F1EA",
          fontSize: "11px",
          cursor: "pointer",
        }}
      >
        Spawn NPC / Monster Here...
      </button>
    </div>
  );
}

export function EntityInspector(
  props: EntityInspectorProps,
): React.ReactElement | null {
  const storeSnap = useWorldStreamStore();
  if (!props.entity && (!props.viewState || props.viewState === "empty")) {
    return null;
  }
  if (
    props.entity &&
    (storeSnap.pickedCoordinate !== null || storeSnap.selectedCityNodeName !== null) &&
    storeSnap.selectedEntityGuid === null &&
    storeSnap.selectedPoiGuid === null
  ) {
    return null;
  }
  const model = buildEntityInspectorModel(props);
  return (
    <aside
      aria-label="Selected Entity Inspector"
      data-testid="verra-entity-inspector"
      data-view-state={model.viewState}
      style={{
        position: "relative",
        width: "100%",
        boxSizing: "border-box",
        padding: "12px",
        borderRadius: "8px",
        border: "1px solid rgba(140, 108, 66, 0.47)",
        background: "linear-gradient(135deg, rgba(11, 14, 23, 0.78) 0%, rgba(16, 23, 36, 0.84) 100%)",
        backdropFilter: "blur(16px)",
        color: "#F3EFE6",
        fontSize: "12px",
        fontVariantNumeric: "tabular-nums",
        zIndex: 20,
      }}
    >
      <div style={{ display: "flex", justifyContent: "space-between", marginBottom: "8px" }}>
        <strong>{props.entity?.name ?? "Entity Inspector"}</strong>
        {props.onClose ? (
          <button
            type="button"
            aria-label="Close entity inspector"
            onClick={props.onClose}
            style={{ minWidth: "24px", minHeight: "24px", border: "1px solid #30363D", borderRadius: "4px", backgroundColor: "#21262D", color: "#E6EDF3", cursor: "pointer" }}
          >
            Close
          </button>
        ) : null}
      </div>
      {model.statusMessage ? (
        <div role={model.viewState === "error" ? "alert" : "status"} data-testid="inspector-view-state-banner" style={{ marginBottom: "8px", color: model.viewState === "error" ? "#FFA198" : "#E3B341" }}>
          {model.statusMessage}
        </div>
      ) : null}
      <div role="grid" aria-label="Entity Telemetry Grid" data-testid="entity-inspector-grid">
        {model.rows.map((row, idx) => (
          <div key={row.rowId} role="row" tabIndex={idx === model.activeRowIndex ? 0 : -1} style={{ display: "flex", justifyContent: "space-between", padding: "2px 0", borderBottom: "1px solid #21262D" }}>
            <span role="gridcell" style={{ color: "#8B949E" }}>{row.label}</span>
            <span role="gridcell" style={{ color: "#E6EDF3", fontWeight: 500 }}>{row.value}</span>
          </div>
        ))}
      </div>
      {props.entity && (props.showQuickActions ?? !props.onClose)
        ? renderEntityInspectorActionButtons(props.entity, storeSnap)
        : null}
    </aside>
  );
}

export default EntityInspector;
