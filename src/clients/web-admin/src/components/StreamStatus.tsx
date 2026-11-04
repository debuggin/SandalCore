// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import {
  type CameraMode,
  type StreamConnectionState,
  STREAM_TARGET_HZ,
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
  gap: "12px",
  padding: "6px 12px",
  borderRadius: "6px",
  backgroundColor: "#0D1117",
  color: "#E6EDF3",
  fontFamily: "ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace",
  fontSize: "12px",
  lineHeight: 1.4,
  fontVariantNumeric: "tabular-nums",
});

const BADGE_BASE_STYLE: React.CSSProperties = Object.freeze({
  display: "inline-flex",
  alignItems: "center",
  gap: "6px",
  padding: "2px 8px",
  borderRadius: "4px",
  fontWeight: 600,
});

const TOGGLE_BUTTON_BASE_STYLE: React.CSSProperties = Object.freeze({
  minWidth: "44px",
  minHeight: "28px",
  padding: "4px 10px",
  borderRadius: "4px",
  border: "1px solid #30363D",
  color: "#F0F6FC",
  fontWeight: 600,
  fontSize: "12px",
  cursor: "pointer",
});

function formatFiniteInt(value: number | undefined, fallback = 0): string {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    return String(fallback);
  }
  const rounded = Math.max(0, Math.round(value));
  return Object.is(rounded, -0) ? "0" : String(rounded);
}

function formatFiniteMs(value: number | undefined): string {
  if (typeof value !== "number" || !Number.isFinite(value) || value < 0) {
    return "0.0 ms";
  }
  const clamped = Object.is(value, -0) ? 0 : value;
  return `${clamped.toFixed(1)} ms`;
}

// Redundant Color + Shape + Text encoding satisfies MIL-STD-1472H & WCAG 1.4.1 non-color telemetry laws.
function resolveConnectionVisuals(
  connectionState: StreamConnectionState,
  hz: number,
): Pick<
  StreamStatusVisualDescriptor,
  "badgeLabel" | "glyph" | "toneColor" | "borderColor" | "backgroundColor"
> {
  const safeHz =
    typeof hz === "number" && Number.isFinite(hz) && hz > 0
      ? Math.round(hz)
      : STREAM_TARGET_HZ;
  switch (connectionState) {
    case "live":
      return {
        badgeLabel: `LIVE ${safeHz}Hz`,
        glyph: "\u25CF",
        toneColor: "#3FB950",
        borderColor: "#238636",
        backgroundColor: "rgba(35, 134, 54, 0.16)",
      };
    case "degraded":
      return {
        badgeLabel: "DEGRADED",
        glyph: "\u25B2",
        toneColor: "#D29922",
        borderColor: "#9E6A03",
        backgroundColor: "rgba(158, 106, 3, 0.18)",
      };
    case "replaying":
      return {
        badgeLabel: `REPLAY ${safeHz}Hz`,
        glyph: "\u25B6",
        toneColor: "#58A6FF",
        borderColor: "#1F6FEB",
        backgroundColor: "rgba(31, 111, 235, 0.18)",
      };
    case "connecting":
      return {
        badgeLabel: "CONNECTING",
        glyph: "\u25CC",
        toneColor: "#8B949E",
        borderColor: "#30363D",
        backgroundColor: "rgba(48, 54, 61, 0.25)",
      };
    case "offline":
    default:
      return {
        badgeLabel: "OFFLINE",
        glyph: "\u25A0",
        toneColor: "#F85149",
        borderColor: "#DA3633",
        backgroundColor: "rgba(218, 54, 51, 0.18)",
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
  const streamHz = props.streamHz ?? state.streamHz;
  const cameraMode = props.cameraMode ?? state.camera.mode;
  const visuals = resolveConnectionVisuals(connectionState, streamHz);
  const nextCameraMode: CameraMode = cameraMode === "2d" ? "3d" : "2d";

  return {
    ...visuals,
    formattedEntityCount: formatFiniteInt(entityCount, 0),
    formattedSeq: `#${formatFiniteInt(lastSeq, 0)}`,
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
  const cameraMode = props.cameraMode ?? storeSnap.camera.mode;
  const needsResync = props.needsSnapshotResync ?? storeSnap.needsSnapshotResync;

  const handleToggleCameraMode = (): void => {
    if (props.onCameraModeChange) {
      props.onCameraModeChange(descriptor.nextCameraMode);
      return;
    }
    storeSnap.setCameraMode(descriptor.nextCameraMode);
  };

  return (
    <div
      role="status"
      aria-live="polite"
      aria-atomic="true"
      aria-label={`World stream status: ${descriptor.badgeLabel}`}
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
      <span data-testid="stream-entity-count" title="Active tracked entities">
        Entities: <strong>{descriptor.formattedEntityCount}</strong>
      </span>
      <span data-testid="stream-sequence-number" title="Last world delta sequence">
        Seq: <strong>{descriptor.formattedSeq}</strong>
      </span>
      <span
        data-testid="stream-tick-latency"
        style={{ color: descriptor.latencyWarn ? "#D29922" : "#8B949E" }}
        title="World stream tick latency"
      >
        Tick: <strong>{descriptor.formattedLatencyMs}</strong>
      </span>
      {needsResync ? (
        <span role="alert" data-testid="stream-resync-alert" style={{ color: "#F85149", fontWeight: 600 }}>
          RESYNC REQUIRED
        </span>
      ) : null}
      <button
        type="button"
        data-testid="camera-mode-toggle"
        aria-pressed={cameraMode === "3d"}
        aria-label={`Toggle 2D or 3D camera mode (current: ${descriptor.cameraToggleLabel})`}
        onClick={handleToggleCameraMode}
        style={{
          ...TOGGLE_BUTTON_BASE_STYLE,
          backgroundColor: cameraMode === "3d" ? "#1F6FEB" : "#21262D",
        }}
      >
        {cameraMode.toUpperCase()}
      </button>
    </div>
  );
}

export default StreamStatus;
