// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import { worldCmToMosaicPx } from "../state/worldStreamStore";

export const K_CAPSULE_HALF_HEIGHT_CM = 96.0;

export interface SpatialPointCm {
  readonly xCm: number;
  readonly yCm: number;
  readonly zCm: number;
}

export interface GroundSnappedTeleportPayload {
  readonly command: "Teleport";
  readonly target_guid: string | null;
  readonly x: number;
  readonly y: number;
  readonly has_z: false;
  readonly capsule_half_height_cm: number;
  readonly snap_formula: "ground_at(x, y) + kCapsuleHalfHeightCm";
}

export interface SpawnNpcSpatialPayload {
  readonly command: "SpawnNpc";
  readonly npc_template_id: string;
  readonly x: number;
  readonly y: number;
  readonly z: number;
  readonly level: number;
}

export interface MoveNpcHomePayload {
  readonly command: "MoveNpc";
  readonly npc_guid: string;
  readonly home_x: number;
  readonly home_y: number;
  readonly home_z: number;
  readonly update_spawn_anchor: true;
}

export interface BvhLosProbeMetrics {
  readonly hit_t?: number;
  readonly occluded?: boolean;
  readonly material_mask?: number;
}

export interface RaycastLosSpatialPayload {
  readonly command: "RaycastLos";
  readonly origin_x: number;
  readonly origin_y: number;
  readonly origin_z: number;
  readonly target_x: number;
  readonly target_y: number;
  readonly target_z: number;
  readonly hit_t: number;
  readonly occluded: boolean;
  readonly material_mask: number;
}

export type WorldContextActionId =
  | "Teleport"
  | "SpawnNpc"
  | "OpenSpawnerTab"
  | "MoveNpc"
  | "RaycastLos"
  | "Dismiss";

export interface WorldContextMenuItem {
  readonly id: WorldContextActionId;
  readonly label: string;
  readonly disabled: boolean;
}

export interface ContextMenuKeyResult {
  readonly nextIndex: number;
  readonly dismissed: boolean;
}

export interface WorldContextMenuProps {
  readonly picked: SpatialPointCm | null;
  readonly selectedPlayer?: string;
  readonly selectedEntityGuid?: string | null;
  readonly selectedEntityOrigin?: SpatialPointCm | null;
  readonly focusedIndex?: number;
  readonly statusNote?: string | null;
  readonly onAction?: (
    action: WorldContextActionId,
    payload: Readonly<Record<string, unknown>> | null,
  ) => void;
  readonly onDismiss: () => void;
}

function assertFiniteFloat64(value: number, fieldName: string): number {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    throw new RangeError(`${fieldName} must be a finite Float64 number`);
  }
  return value;
}

function assertSpatialPointFinite(point: SpatialPointCm, prefix: string): void {
  assertFiniteFloat64(point.xCm, `${prefix}.xCm`);
  assertFiniteFloat64(point.yCm, `${prefix}.yCm`);
  assertFiniteFloat64(point.zCm, `${prefix}.zCm`);
}

export function buildGroundSnappedTeleportPayload(
  point: SpatialPointCm,
  targetGuid: string | null = null,
): GroundSnappedTeleportPayload {
  const x = assertFiniteFloat64(point.xCm, "point.xCm");
  const y = assertFiniteFloat64(point.yCm, "point.yCm");
  assertFiniteFloat64(point.zCm, "point.zCm");
  return {
    command: "Teleport",
    target_guid: targetGuid,
    x,
    y,
    has_z: false,
    capsule_half_height_cm: K_CAPSULE_HALF_HEIGHT_CM,
    snap_formula: "ground_at(x, y) + kCapsuleHalfHeightCm",
  };
}

export function buildSpawnNpcPayload(
  npcTemplateId: string,
  point: SpatialPointCm,
  level = 50,
): SpawnNpcSpatialPayload {
  if (!npcTemplateId || npcTemplateId.trim().length === 0) {
    throw new RangeError("npcTemplateId must be non-empty");
  }
  assertSpatialPointFinite(point, "point");
  const cleanLevel = Math.max(
    1,
    Math.min(80, Math.trunc(assertFiniteFloat64(level, "level"))),
  );
  return {
    command: "SpawnNpc",
    npc_template_id: npcTemplateId.trim(),
    x: point.xCm,
    y: point.yCm,
    z: point.zCm,
    level: cleanLevel,
  };
}

export function buildMoveNpcPayload(
  npcGuid: string,
  destination: SpatialPointCm,
): MoveNpcHomePayload {
  if (!npcGuid || npcGuid.trim().length === 0) {
    throw new RangeError("npcGuid must be non-empty for MoveNpc relocation");
  }
  assertSpatialPointFinite(destination, "destination");
  return {
    command: "MoveNpc",
    npc_guid: npcGuid.trim(),
    home_x: destination.xCm,
    home_y: destination.yCm,
    home_z: destination.zCm,
    update_spawn_anchor: true,
  };
}

export function buildRaycastLosPayload(
  origin: SpatialPointCm,
  target: SpatialPointCm,
  probe: BvhLosProbeMetrics = {},
): RaycastLosSpatialPayload {
  assertSpatialPointFinite(origin, "origin");
  assertSpatialPointFinite(target, "target");
  const hitT =
    probe.hit_t !== undefined
      ? assertFiniteFloat64(probe.hit_t, "probe.hit_t")
      : 1.0;
  if (hitT < 0 || hitT > 1) {
    throw new RangeError("BVH raycast hit_t must be within [0.0, 1.0]");
  }
  const mask =
    probe.material_mask !== undefined
      ? Math.trunc(
          assertFiniteFloat64(probe.material_mask, "probe.material_mask"),
        ) >>> 0
      : 0x01;
  return {
    command: "RaycastLos",
    origin_x: origin.xCm,
    origin_y: origin.yCm,
    origin_z: origin.zCm,
    target_x: target.xCm,
    target_y: target.yCm,
    target_z: target.zCm,
    hit_t: hitT,
    occluded: probe.occluded ?? hitT < 1.0,
    material_mask: mask,
  };
}

export function navigateContextMenuKey(
  currentIndex: number,
  itemCount: number,
  key: string,
): ContextMenuKeyResult {
  if (itemCount <= 0) {
    return { nextIndex: 0, dismissed: key === "Escape" };
  }
  const clamped = Math.min(Math.max(0, currentIndex), itemCount - 1);
  if (key === "Escape") return { nextIndex: clamped, dismissed: true };
  if (key === "ArrowDown") {
    return { nextIndex: (clamped + 1) % itemCount, dismissed: false };
  }
  if (key === "ArrowUp") {
    return {
      nextIndex: (clamped - 1 + itemCount) % itemCount,
      dismissed: false,
    };
  }
  if (key === "Home") return { nextIndex: 0, dismissed: false };
  if (key === "End") return { nextIndex: itemCount - 1, dismissed: false };
  return { nextIndex: clamped, dismissed: false };
}

export function WorldContextMenu(
  props: WorldContextMenuProps,
): React.ReactElement | null {
  if (!props.picked || props.selectedEntityGuid) {
    return null;
  }
  const [mosaicU, mosaicV] = worldCmToMosaicPx(
    props.picked.xCm,
    props.picked.yCm,
  );
  const items: readonly WorldContextMenuItem[] = [
    {
      id: "Teleport",
      label: "Teleport Player Here...",
      disabled: false,
    },
    { id: "SpawnNpc", label: "Spawn NPC / Monster Here...", disabled: false },
    {
      id: "OpenSpawnerTab",
      label: "Open in Players & Spawner Tab",
      disabled: false,
    },
    { id: "Dismiss", label: "Dismiss", disabled: false },
  ];
  const focusIdx = Math.min(
    Math.max(0, props.focusedIndex ?? 0),
    items.length - 1,
  );
  return (
    <div
      role="menu"
      aria-label="Picked Verra Unreal Coordinate Context Menu"
      data-testid="verra-coordinate-context-menu"
      style={{
        position: "absolute",
        left: "12px",
        top: "12px",
        padding: "10px 12px",
        borderRadius: "6px",
        border: "1px solid #30363D",
        backgroundColor: "rgba(13, 17, 23, 0.96)",
        color: "#F0F6FC",
        fontSize: "12px",
        fontVariantNumeric: "tabular-nums",
        zIndex: 25,
      }}
    >
      <div>
        <strong>Picked Unreal Coordinate (cm)</strong>
      </div>
      <div
        data-testid="picked-coordinate-readout"
        style={{ marginBottom: "2px", color: "#C9D1D9" }}
      >
        X: {Math.round(props.picked.xCm)} | Y: {Math.round(props.picked.yCm)} | Z:{" "}
        {Math.round(props.picked.zCm)}
      </div>
      <div style={{ marginBottom: "6px", color: "#8B949E", fontSize: "11px" }}>
        Mosaic px: ({Math.round(mosaicU)}, {Math.round(mosaicV)})
      </div>
      {props.statusNote ? (
        <div
          style={{
            marginBottom: "6px",
            color: "#58A6FF",
            fontSize: "11px",
          }}
        >
          {props.statusNote}
        </div>
      ) : null}
      {items.map((item, idx) => (
        <button
          key={item.id}
          type="button"
          role="menuitem"
          disabled={item.disabled}
          tabIndex={idx === focusIdx ? 0 : -1}
          data-testid={
            item.id === "Dismiss"
              ? "dismiss-picked-coordinate"
              : `context-menu-${item.id}`
          }
          onClick={() =>
            item.id === "Dismiss"
              ? props.onDismiss()
              : props.onAction?.(item.id, null)
          }
          style={{
            display: "block",
            width: "100%",
            minHeight: "26px",
            marginTop: "4px",
            padding: "4px 8px",
            textAlign: "left",
            borderRadius: "4px",
            border: "1px solid #30363D",
            backgroundColor: idx === focusIdx ? "#1F6FEB" : "#21262D",
            color: item.disabled ? "#6E7681" : "#F0F6FC",
            cursor: item.disabled ? "not-allowed" : "pointer",
          }}
        >
          {item.label}
        </button>
      ))}
    </div>
  );
}

export default WorldContextMenu;
