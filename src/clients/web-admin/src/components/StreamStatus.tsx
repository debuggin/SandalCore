// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import {
  type CameraMode,
  computeLayerEntityCounts,
  type StreamConnectionState,
  UI_FRAME_BUDGET_MS,
  useWorldStreamStore,
  type WorldStreamState,
} from "../state/worldStreamStore";

export interface StreamStatusProps {
  readonly connectionState?: StreamConnectionState;
  readonly entityCount?: number;
  readonly lastSeq?: number;
  readonly tickLatencyMs?: number;
  readonly streamHz?: number;
  readonly cameraMode?: CameraMode;
  readonly needsSnapshotResync?: boolean;
  readonly onCameraModeChange?: (nextMode: CameraMode) => void;
}

export interface StreamStatusVisualDescriptor {
  readonly badgeLabel: string;
  readonly glyph: string;
  readonly toneColor: string;
  readonly borderColor: string;
  readonly backgroundColor: string;
  readonly formattedEntityCount: string;
  readonly formattedSeq: string;
  readonly formattedLatencyMs: string;
  readonly latencyWarn: boolean;
  readonly nextCameraMode: CameraMode;
  readonly cameraToggleLabel: string;
}

const CONTAINER_BASE_STYLE: React.CSSProperties = Object.freeze({
  display: "inline-flex",
  alignItems: "center",
  gap: "10px",
  padding: "5px 12px",
  borderRadius: "6px",
  background: "linear-gradient(135deg, rgba(11, 14, 23, 0.78) 0%, rgba(16, 23, 36, 0.84) 100%)",
  backdropFilter: "blur(16px)",
  color: "#F4F1EA",
  fontFamily: '"Segoe UI", system-ui, -apple-system, sans-serif',
  fontSize: "11px",
  fontWeight: 700,
  lineHeight: 1.4,
});

const BADGE_BASE_STYLE: React.CSSProperties = Object.freeze({
  display: "inline-flex",
  alignItems: "center",
  gap: "6px",
  padding: "2px 8px",
  borderRadius: "999px",
  fontWeight: 600,
});

function formatFiniteInt(value: number | undefined, fallback = 0): string {
  if (typeof value !== "number" || !Number.isFinite(value)) return String(fallback);
  const rounded = Math.max(0, Math.round(value));
  return Object.is(rounded, -0) ? "0" : String(rounded);
}

function formatFiniteMs(value: number | undefined): string {
  if (typeof value !== "number" || !Number.isFinite(value) || value < 0) return "0.0 ms";
  const clamped = Object.is(value, -0) ? 0 : value;
  return `${clamped.toFixed(1)} ms`;
}

function resolveConnectionVisuals(
  connectionState: StreamConnectionState,
  _hz: number,
): Pick<
  StreamStatusVisualDescriptor,
  "badgeLabel" | "glyph" | "toneColor" | "borderColor" | "backgroundColor"
> {
  switch (connectionState) {
    case "live":
      return {
        badgeLabel: "ONLINE",
        glyph: "\u25CF",
        toneColor: "#20D8CE",
        borderColor: "rgba(32, 216, 206, 0.45)",
        backgroundColor: "rgba(32, 216, 206, 0.14)",
      };
    case "degraded":
      return {
        badgeLabel: "DEGRADED",
        glyph: "\u25B2",
        toneColor: "#F5C469",
        borderColor: "rgba(245, 196, 105, 0.45)",
        backgroundColor: "rgba(245, 196, 105, 0.16)",
      };
    case "replaying":
      return {
        badgeLabel: "REPLAY",
        glyph: "\u25B6",
        toneColor: "#E8B25A",
        borderColor: "rgba(232, 178, 90, 0.47)",
        backgroundColor: "rgba(232, 178, 90, 0.16)",
      };
    case "connecting":
      return {
        badgeLabel: "CONNECTING",
        glyph: "\u25CC",
        toneColor: "#B8C4D4",
        borderColor: "rgba(140, 108, 66, 0.47)",
        backgroundColor: "rgba(22, 30, 46, 0.65)",
      };
    case "offline":
    default:
      return {
        badgeLabel: "OFFLINE",
        glyph: "\u25A0",
        toneColor: "#E05258",
        borderColor: "rgba(224, 82, 88, 0.5)",
        backgroundColor: "rgba(224, 82, 88, 0.16)",
      };
  }
}

export function buildStreamStatusDescriptor(
  props: StreamStatusProps,
  storeSnap?: WorldStreamState,
): StreamStatusVisualDescriptor {
  const state = storeSnap ?? useWorldStreamStore.getState();
  const connectionState = props.connectionState ?? state.connectionState;
  const entityCount = props.entityCount ?? state.entities.size;
  const lastSeq = props.lastSeq ?? state.lastSeq;
  const tickLatencyMs = props.tickLatencyMs ?? state.tickLatencyMs;
  const effectivePollHz = 30;
  const streamHz = props.streamHz ?? effectivePollHz;
  const cameraMode = props.cameraMode ?? state.camera.mode;
  const visuals = resolveConnectionVisuals(connectionState, streamHz);
  const nextCameraMode: CameraMode = cameraMode === "2d" ? "3d" : "2d";

  return {
    ...visuals,
    formattedEntityCount: formatFiniteInt(entityCount, 0),
    formattedSeq: formatFiniteInt(lastSeq, 0),
    formattedLatencyMs: formatFiniteMs(tickLatencyMs),
    latencyWarn:
      typeof tickLatencyMs === "number" &&
      Number.isFinite(tickLatencyMs) &&
      tickLatencyMs > UI_FRAME_BUDGET_MS,
    nextCameraMode,
    cameraToggleLabel: cameraMode === "3d" ? "3D Perspective" : "2D Orthographic",
  };
}

export function StreamStatus(props: StreamStatusProps): React.ReactElement {
  const storeSnap = useWorldStreamStore();
  const descriptor = buildStreamStatusDescriptor(props, storeSnap);
  const counts = computeLayerEntityCounts(storeSnap);
  const needsResync = props.needsSnapshotResync ?? storeSnap.needsSnapshotResync;

  return (
    <div
      role="status"
      aria-live="polite"
      aria-atomic="true"
      aria-label={`World status: ${descriptor.badgeLabel}`}
      data-testid="verra-stream-status"
      data-connection-state={props.connectionState ?? storeSnap.connectionState}
      style={{ ...CONTAINER_BASE_STYLE, border: `1px solid ${descriptor.borderColor}` }}
    >
      <span
        data-testid="stream-state-badge"
        style={{
          ...BADGE_BASE_STYLE,
          color: descriptor.toneColor,
          backgroundColor: descriptor.backgroundColor,
        }}
      >
        <span aria-hidden="true">{descriptor.glyph}</span>
        <span>{descriptor.badgeLabel}</span>
      </span>
      <span data-testid="stream-entity-count" style={{ color: "#F4F1EA", fontFamily: "inherit", fontSize: "11px", fontWeight: 700 }} title="Online players in world">
        {`Players Online: ${counts.players}`}
      </span>
      {needsResync ? (
        <span role="alert" data-testid="stream-resync-alert" style={{ color: "#E05258", fontWeight: 600 }}>
          RESYNCING
        </span>
      ) : null}
    </div>
  );
}

export default StreamStatus;
