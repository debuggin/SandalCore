import React from "react";
import { verraUnrealToLngLat, type WorldEntity } from "../state/worldStreamStore";

export type InspectorViewState = "loading" | "empty" | "error" | "degraded" | "populated";

export interface HotfixOverlayEntry {
  readonly overlay_id?: number;
  readonly target_table: string;
  readonly record_guid: number | string;
  readonly field_path: string;
  readonly old_value: unknown;
  readonly new_value: unknown;
  readonly enabled?: boolean;
  readonly reason?: string;
}

export interface HotfixOverlayDiffRow {
  readonly key: string;
  readonly target_table: string;
  readonly record_guid: string;
  readonly field_path: string;
  readonly oldFormatted: string;
  readonly newFormatted: string;
  readonly diffSummary: string;
  readonly summary: string;
  readonly changed: boolean;
}

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
  readonly hotfixOverlays?: readonly HotfixOverlayEntry[];
}

export interface EntityInspectorProps {
  readonly entity: InspectedWorldEntity | null;
  readonly viewState?: InspectorViewState;
  readonly hotfixOverlays?: readonly HotfixOverlayEntry[];
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
  readonly hotfixDiffs: readonly HotfixOverlayDiffRow[];
  readonly statusMessage: string | null;
}

export const INSPECTOR_CONTRAST_RATIO_AA = 15.8;

function formatSafeValue(raw: unknown): string {
  if (raw === null || raw === undefined) {
    return "\u2014";
  }
  if (typeof raw === "number") {
    return Number.isFinite(raw) ? String(raw) : "\u2014";
  }
  if (typeof raw === "boolean" || typeof raw === "bigint") {
    return String(raw);
  }
  if (typeof raw === "string") {
    return raw.trim().length > 0 ? raw : "\u2014";
  }
  return JSON.stringify(raw);
}

export function computeHotfixOverlayDiffs(
  overlays: readonly HotfixOverlayEntry[] | undefined,
): readonly HotfixOverlayDiffRow[] {
  if (!overlays || overlays.length === 0) {
    return [];
  }
  const rows: HotfixOverlayDiffRow[] = [];
  for (const entry of overlays) {
    if (entry.enabled === false) {
      continue;
    }
    const targetTable = formatSafeValue(entry.target_table);
    const recordGuid = formatSafeValue(entry.record_guid);
    const fieldPath = formatSafeValue(entry.field_path);
    const oldFormatted = formatSafeValue(entry.old_value);
    const newFormatted = formatSafeValue(entry.new_value);
    const summary = `${oldFormatted} -> ${newFormatted}`;
    rows.push({
      key: `${targetTable}:${recordGuid}:${fieldPath}`,
      target_table: targetTable,
      record_guid: recordGuid,
      field_path: fieldPath,
      oldFormatted,
      newFormatted,
      diffSummary: summary,
      summary,
      changed: oldFormatted !== newFormatted,
    });
  }
  return rows;
}

export function moveInspectorGridRowFocus(currentIndex: number, rowCount: number, key: string): number {
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

function buildEntityAttributeRows(entity: InspectedWorldEntity | null): readonly InspectorGridRow[] {
  if (!entity) {
    return [];
  }
  const [lng, lat] = verraUnrealToLngLat(entity.xCm, entity.yCm);
  const pct = Number.isFinite(entity.healthPct) ? entity.healthPct.toFixed(1) : "0.0";
  const hasHp = typeof entity.hp === "number" && typeof entity.maxHp === "number" && Number.isFinite(entity.hp) && Number.isFinite(entity.maxHp);
  const hpText = hasHp ? `${Math.round(entity.hp!)} / ${Math.round(entity.maxHp!)} (${pct}%)` : `${pct}%`;
  const buffs = entity.activeBuffs && entity.activeBuffs.length > 0 ? entity.activeBuffs.join(", ") : "None";
  return [
    { rowId: "guid", label: "GUID", value: formatSafeValue(entity.guid) },
    { rowId: "name", label: "Name", value: formatSafeValue(entity.name) },
    { rowId: "kind", label: "Kind", value: formatSafeValue(entity.kind) },
    { rowId: "level", label: "Level", value: formatSafeValue(entity.level) },
    { rowId: "hp", label: "HP / MaxHP", value: hpText },
    { rowId: "unreal_cm", label: "Unreal (cm)", value: `X=${entity.xCm.toFixed(2)}, Y=${entity.yCm.toFixed(2)}, Z=${entity.zCm.toFixed(2)}` },
    { rowId: "lng_lat", label: "WGS84 (lng, lat)", value: `${lng.toFixed(6)}, ${lat.toFixed(6)}` },
    { rowId: "state_flags", label: "State Flags", value: resolveStateFlagsList(entity) },
    { rowId: "active_buffs", label: "Active Buffs", value: buffs },
  ];
}

function resolveStatusMessage(viewState: InspectorViewState, errorDetail?: string): string | null {
  if (viewState === "loading") return "Loading entity telemetry and hotfix overlays...";
  if (viewState === "empty") return "No entity selected. Select an entity on the Verra canvas or list.";
  if (viewState === "error") return errorDetail ?? "Entity inspection stream failed. Retry or resync snapshot.";
  if (viewState === "degraded") return "Stale telemetry: stream sequence gap detected, awaiting snapshot resync.";
  return null;
}

export function buildEntityInspectorModel(props: EntityInspectorProps): EntityInspectorModel {
  const rows = buildEntityAttributeRows(props.entity);
  const hotfixDiffs = computeHotfixOverlayDiffs(props.hotfixOverlays ?? props.entity?.hotfixOverlays);
  const viewState: InspectorViewState = props.viewState ?? (props.entity ? "populated" : "empty");
  const activeRowIndex = Math.max(0, Math.min(Math.max(0, rows.length - 1), props.activeRowIndex ?? 0));
  return {
    role: "grid",
    viewState,
    activeRowIndex,
    contrastRatioMin: INSPECTOR_CONTRAST_RATIO_AA,
    rows,
    hotfixDiffs,
    statusMessage: resolveStatusMessage(viewState, props.errorDetail),
  };
}

export function EntityInspector(props: EntityInspectorProps): React.ReactElement | null {
  if (!props.entity && (!props.viewState || props.viewState === "empty")) {
    return null;
  }
  const model = buildEntityInspectorModel(props);
  return (
    <aside
      aria-label="Selected Entity Inspector"
      data-testid="verra-entity-inspector"
      data-view-state={model.viewState}
      style={{ position: "absolute", top: "56px", right: "12px", width: "320px", padding: "12px", borderRadius: "6px", border: "1px solid #30363D", backgroundColor: "#0D1117", color: "#E6EDF3", fontSize: "12px", fontVariantNumeric: "tabular-nums" }}
    >
      <div style={{ display: "flex", justifyContent: "space-between", marginBottom: "8px" }}>
        <strong>{props.entity?.name ?? "Entity Inspector"}</strong>
        {props.onClose ? (
          <button type="button" aria-label="Close entity inspector" onClick={props.onClose} style={{ minWidth: "24px", minHeight: "24px", border: "1px solid #30363D", borderRadius: "4px", backgroundColor: "#21262D", color: "#E6EDF3", cursor: "pointer" }}>
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
      {model.hotfixDiffs.length > 0 ? (
        <div role="region" aria-label="world_hotfix_overlays diff preview" data-testid="inspector-hotfix-diffs" style={{ marginTop: "8px", paddingTop: "6px", borderTop: "1px solid #30363D" }}>
          <div style={{ color: "#58A6FF", fontWeight: 600, marginBottom: "4px" }}>world_hotfix_overlays</div>
          {model.hotfixDiffs.map((diff) => (
            <div key={diff.key} data-testid={`hotfix-diff-${diff.key}`} style={{ fontSize: "11px" }}>
              <span>{diff.target_table}.{diff.field_path} (#{diff.record_guid}): </span>
              <strong>{diff.diffSummary}</strong>
            </div>
          ))}
        </div>
      ) : null}
    </aside>
  );
}

export default EntityInspector;
