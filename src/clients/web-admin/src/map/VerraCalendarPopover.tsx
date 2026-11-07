// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import { fetchWithTransientRetry } from "../components/GmCommandPalette";
import {
  computeVerraCalendar,
  formatTimeOfDayClock,
  formatTimeOfDayLabel,
  formatTimePhaseLabel,
  isUnauthenticatedViewOnly,
  parseTimeOfDayInput,
  VERRA_CALENDAR_MONTH_NAMES,
  type VerraCalendarSnapshot,
  type WorldStreamState,
} from "../state/worldStreamStore";
import { SANDAL_LAUNCHER_TOKENS } from "../components/PlayersAndSpawnerTab";

export const VERRA_SEASON_PRESETS = Object.freeze([
  { id: "spring", label: "Spring (D1)", dayOfYear: 1 },
  { id: "summer", label: "Summer (D91)", dayOfYear: 91 },
  { id: "autumn", label: "Autumn (D181)", dayOfYear: 181 },
  { id: "winter", label: "Winter (D271)", dayOfYear: 271 },
] as const);

export interface LiveVerraClockState {
  readonly liveHours: number;
  readonly liveClock: string;
  readonly livePhase: string;
  readonly liveCalendar: VerraCalendarSnapshot;
  readonly setLiveAnchor: (hours: number, dayOfYear: number, year: number) => void;
}

export function useLiveVerraClockAndCalendar(storeSnap: WorldStreamState): LiveVerraClockState {
  const lastSyncedAtRef = React.useRef<number>(storeSnap.timeOfDaySyncedAtMs);
  const anchorRef = React.useRef<{ hours: number; dayOfYear: number; year: number; atMs: number }>({
    hours: storeSnap.timeOfDayHours ?? 12,
    dayOfYear: storeSnap.calendarDayOfYear ?? 1,
    year: storeSnap.calendarYear ?? 1,
    atMs: Date.now(),
  });
  if (storeSnap.timeOfDaySyncedAtMs !== lastSyncedAtRef.current) {
    lastSyncedAtRef.current = storeSnap.timeOfDaySyncedAtMs;
    anchorRef.current = {
      hours: storeSnap.timeOfDayHours ?? 12,
      dayOfYear: storeSnap.calendarDayOfYear ?? 1,
      year: storeSnap.calendarYear ?? 1,
      atMs: Date.now(),
    };
  }
  const [nowMs, setNowMs] = React.useState(() => Date.now());
  React.useEffect(() => {
    if (typeof window === "undefined") return;
    const timer = window.setInterval(() => setNowMs(Date.now()), 250);
    return () => window.clearInterval(timer);
  }, []);
  const elapsedGameHours = Math.max(0, ((nowMs - anchorRef.current.atMs) / 3_600_000) * 9);
  const totalH = anchorRef.current.hours + elapsedGameHours;
  const daysAdded = Math.floor(totalH / 24);
  const liveHours = ((totalH % 24) + 24) % 24;
  const liveCalendar = computeVerraCalendar((anchorRef.current.dayOfYear ?? 1) + daysAdded, anchorRef.current.year ?? 1);
  const setLiveAnchor = (hours: number, dayOfYear: number, year: number): void => {
    anchorRef.current = { hours, dayOfYear, year, atMs: Date.now() };
    setNowMs(Date.now());
  };
  return {
    liveHours,
    liveClock: formatTimeOfDayClock(liveHours),
    livePhase: formatTimePhaseLabel(liveHours),
    liveCalendar,
    setLiveAnchor,
  };
}

export function pollAuthoritativeTimeOfDay(storeSnapRef: React.MutableRefObject<WorldStreamState>): void {
  if (typeof window === "undefined" || typeof fetch !== "function" || isUnauthenticatedViewOnly()) return;
  fetchWithTransientRetry("/api/v1/gm/world/time-of-day")
    .then((r) => (r.ok ? r.json() : null))
    .then((data) => {
      if (!data || typeof data !== "object" || isUnauthenticatedViewOnly()) return;
      const d = data as Record<string, unknown>;
      const rawH = typeof d.time_of_day_hours === "number" ? d.time_of_day_hours : typeof d.hours === "number" ? d.hours : typeof d.hour === "number" ? d.hour : null;
      const now = Date.now();
      if (rawH !== null && Number.isFinite(rawH)) storeSnapRef.current.setTimeOfDay(rawH, now);
      const dayOfYear = typeof d.calendar_day_of_year === "number" ? d.calendar_day_of_year : typeof d.day_of_year === "number" ? d.day_of_year : null;
      const year = typeof d.calendar_year === "number" ? d.calendar_year : typeof d.year === "number" ? d.year : 1;
      if (dayOfYear !== null && Number.isFinite(dayOfYear)) storeSnapRef.current.setWorldCalendar(dayOfYear, year, now);
    })
    .catch(() => {});
}

export function dispatchAuthoritativeTimeAndCalendarUpdate(
  storeSnap: WorldStreamState,
  hours: number,
  dayOfYear: number,
  year: number,
): void {
  const cleanHours = (((Number.isFinite(hours) ? hours : 12) % 24) + 24) % 24;
  const cal = computeVerraCalendar(dayOfYear, year);
  void storeSnap.executeSetTimeOfDayAction(cleanHours, { dayOfYear: cal.calendarDayOfYear, year: cal.calendarYear });
  if (typeof fetch === "function") {
    const payload = JSON.stringify({
      command: "set_time_of_day",
      hour: cleanHours,
      hours: cleanHours,
      day_of_year: cal.calendarDayOfYear,
      year: cal.calendarYear,
      params: { hour: cleanHours, hours: cleanHours, day_of_year: cal.calendarDayOfYear, year: cal.calendarYear },
    });
    const headers = { "Content-Type": "application/json", Accept: "application/json" };
    void fetchWithTransientRetry("/api/v1/admin/commands", { method: "POST", headers, body: payload }).catch(() => null);
  }
}

export interface VerraCalendarDateModifierProps {
  readonly draftDayOfYear: number;
  readonly draftYear: number;
  readonly onChangeDate: (dayOfYear: number, year: number) => void;
  readonly onApplyPresetDate: (dayOfYear: number, year: number) => void;
}

export function VerraCalendarDateModifierSection(props: VerraCalendarDateModifierProps): React.ReactElement {
  const { draftDayOfYear, draftYear, onChangeDate, onApplyPresetDate } = props;
  const draftCal = computeVerraCalendar(draftDayOfYear, draftYear);
  const inputStyle: React.CSSProperties = {
    height: "26px", padding: "0 6px", borderRadius: "4px",
    border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
    backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush,
    color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100,
    fontFamily: "ui-monospace, monospace", fontSize: "11px",
  };
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: "6px", paddingTop: "6px", borderTop: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}` }}>
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", fontSize: "11px" }}>
        <strong style={{ color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500, letterSpacing: "0.04em", textTransform: "uppercase" }}>Verra Calendar (360d)</strong>
        <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400, fontSize: "10px", fontWeight: 700 }}>{draftCal.calendarFormatted}</span>
      </div>
      <div style={{ display: "grid", gridTemplateColumns: "repeat(4, 1fr)", gap: "4px" }}>
        {VERRA_SEASON_PRESETS.map((s) => (
          <button key={s.id} type="button" data-testid={`verra-season-preset-${s.id}`} onClick={() => onApplyPresetDate(s.dayOfYear, draftCal.calendarYear)} style={{ height: "24px", padding: "0 4px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "10px", cursor: "pointer" }}>
            {s.label}
          </button>
        ))}
      </div>
      <div style={{ display: "grid", gridTemplateColumns: "74px 1fr 68px", gap: "6px", alignItems: "center" }}>
        <label style={{ display: "flex", flexDirection: "column", gap: "2px", fontSize: "10px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>
          Year (AE)
          <input type="number" min={1} max={9999} aria-label="Verra Calendar Year (AE)" data-testid="verra-calendar-year-input" value={draftCal.calendarYear} onChange={(e) => onChangeDate(draftCal.calendarDayOfYear, Math.max(1, Number(e.currentTarget.value) || 1))} style={inputStyle} />
        </label>
        <label style={{ display: "flex", flexDirection: "column", gap: "2px", fontSize: "10px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>
          Month (1-12)
          <select aria-label="Verra Calendar Month" data-testid="verra-calendar-month-select" value={draftCal.calendarMonth} onChange={(e) => onChangeDate((Math.max(1, Math.min(12, Number(e.currentTarget.value))) - 1) * 30 + draftCal.calendarDayOfMonth, draftCal.calendarYear)} style={inputStyle}>
            {VERRA_CALENDAR_MONTH_NAMES.map((mName, idx) => (<option key={mName} value={idx + 1}>{idx + 1}. {mName}</option>))}
          </select>
        </label>
        <label style={{ display: "flex", flexDirection: "column", gap: "2px", fontSize: "10px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>
          Day (1-30)
          <input type="number" min={1} max={30} aria-label="Verra Calendar Day of Month" data-testid="verra-calendar-day-of-month-input" value={draftCal.calendarDayOfMonth} onChange={(e) => onChangeDate((draftCal.calendarMonth - 1) * 30 + Math.max(1, Math.min(30, Number(e.currentTarget.value) || 1)), draftCal.calendarYear)} style={inputStyle} />
        </label>
      </div>
      <input type="range" min={1} max={360} step={1} aria-label="Verra Calendar Day of Year (1-360)" data-testid="verra-calendar-day-slider" value={draftCal.calendarDayOfYear} onChange={(e) => onChangeDate(Number(e.currentTarget.value), draftCal.calendarYear)} style={{ width: "100%", height: "6px", cursor: "pointer" }} />
    </div>
  );
}

export { formatTimeOfDayLabel, parseTimeOfDayInput };
