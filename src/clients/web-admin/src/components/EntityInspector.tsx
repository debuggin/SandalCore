// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import {
  resolveWorkerForCoordinateOrZone,
  verraUnrealToLngLat,
  worldCmToMosaicPx,
  type WorldEntity,
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
  readonly onClose?: () => void;
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

function buildEntityAttributeRows(
  entity: InspectedWorldEntity | null,
): readonly InspectorGridRow[] {
  if (!entity) {
    return [];
  }
  const [lng, lat] = verraUnrealToLngLat(entity.xCm, entity.yCm);
  const [mosaicU, mosaicV] = worldCmToMosaicPx(entity.xCm, entity.yCm);
  const pct = Number.isFinite(entity.healthPct)
    ? entity.healthPct.toFixed(1)
    : "0.0";
  const hasHp =
    typeof entity.hp === "number" &&
    typeof entity.maxHp === "number" &&
    Number.isFinite(entity.hp) &&
    Number.isFinite(entity.maxHp);
  const hpText = hasHp
    ? `${Math.round(entity.hp!)} / ${Math.round(entity.maxHp!)} (${pct}%)`
    : `${pct}%`;
  const buffs =
    entity.activeBuffs && entity.activeBuffs.length > 0
      ? entity.activeBuffs.join(", ")
      : "None";
  const worker = resolveWorkerForCoordinateOrZone(
    entity.xCm,
    entity.yCm,
    entity.zoneId,
    undefined,
    entity.name,
  );
  return [
    { rowId: "guid", label: "GUID", value: formatSafeValue(entity.guid) },
    { rowId: "name", label: "Name", value: formatSafeValue(entity.name) },
    { rowId: "kind", label: "Kind", value: formatSafeValue(entity.kind) },
    {
      rowId: "worker",
      label: "Server Worker",
      value: `${worker.worker_id} · ${worker.region_label}`,
    },
    { rowId: "level", label: "Level", value: formatSafeValue(entity.level) },
    { rowId: "hp", label: "HP / MaxHP", value: hpText },
    {
      rowId: "unreal_cm",
      label: "Unreal (cm)",
      value: `X=${entity.xCm.toFixed(1)}, Y=${entity.yCm.toFixed(1)}, Z=${entity.zCm.toFixed(1)}`,
    },
    {
      rowId: "mosaic_px",
      label: "Mosaic (px)",
      value: `U=${mosaicU.toFixed(1)}, V=${mosaicV.toFixed(1)}`,
    },
    {
      rowId: "lng_lat",
      label: "WGS84 (lng, lat)",
      value: `${lng.toFixed(5)}, ${lat.toFixed(5)}`,
    },
    {
      rowId: "state_flags",
      label: "State / Buffs",
      value: `${resolveStateFlagsList(entity)} (${buffs})`,
    },
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

export function EntityInspector(
  props: EntityInspectorProps,
): React.ReactElement | null {
  if (!props.entity && (!props.viewState || props.viewState === "empty")) {
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
      <div
        style={{
          display: "flex",
          justifyContent: "space-between",
          marginBottom: "8px",
        }}
      >
        <strong>{props.entity?.name ?? "Entity Inspector"}</strong>
        {props.onClose ? (
          <button
            type="button"
            aria-label="Close entity inspector"
            onClick={props.onClose}
            style={{
              minWidth: "24px",
              minHeight: "24px",
              border: "1px solid #30363D",
              borderRadius: "4px",
              backgroundColor: "#21262D",
              color: "#E6EDF3",
              cursor: "pointer",
            }}
          >
            Close
          </button>
        ) : null}
      </div>
      {model.statusMessage ? (
        <div
          role={model.viewState === "error" ? "alert" : "status"}
          data-testid="inspector-view-state-banner"
          style={{
            marginBottom: "8px",
            color: model.viewState === "error" ? "#FFA198" : "#E3B341",
          }}
        >
          {model.statusMessage}
        </div>
      ) : null}
      <div
        role="grid"
        aria-label="Entity Telemetry Grid"
        data-testid="entity-inspector-grid"
      >
        {model.rows.map((row, idx) => (
          <div
            key={row.rowId}
            role="row"
            tabIndex={idx === model.activeRowIndex ? 0 : -1}
            style={{
              display: "flex",
              justifyContent: "space-between",
              padding: "2px 0",
              borderBottom: "1px solid #21262D",
            }}
          >
            <span role="gridcell" style={{ color: "#8B949E" }}>
              {row.label}
            </span>
            <span role="gridcell" style={{ color: "#E6EDF3", fontWeight: 500 }}>
              {row.value}
            </span>
          </div>
        ))}
      </div>
    </aside>
  );
}

export default EntityInspector;
