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
  VERRA_CALENDAR_SEASON_NAMES,
  type VerraCalendarSnapshot,
  type WorldStreamState,
} from "../state/worldStreamStore";
import { SANDAL_LAUNCHER_TOKENS } from "../components/PlayersAndSpawnerTab";

export const VERRA_SEASON_PRESETS = Object.freeze([
  { id: "spring", label: "Spring", dayOfYear: 1, tooltip: "Starts on Day 1 of 360" },
  { id: "summer", label: "Summer", dayOfYear: 91, tooltip: "Starts on Day 91 of 360" },
  { id: "autumn", label: "Autumn", dayOfYear: 181, tooltip: "Starts on Day 181 of 360" },
  { id: "winter", label: "Winter", dayOfYear: 271, tooltip: "Starts on Day 271 of 360" },
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
    width: "100%", height: "26px", padding: "0 6px", boxSizing: "border-box", borderRadius: "4px",
    border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
    backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush,
    color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100,
    colorScheme: "dark", fontFamily: "inherit", fontSize: "11px", outline: "none",
  };
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: "6px", paddingTop: "6px", borderTop: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}` }}>
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", fontSize: "11px" }}>
        <strong title="360-day year divided into four 90-day seasons" style={{ color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500, letterSpacing: "0.04em", textTransform: "uppercase" }}>Verra Calendar</strong>
        <span title={`Day ${draftCal.calendarDayOfSeason} of 90 in ${draftCal.calendarSeason} (Day ${draftCal.calendarDayOfYear} of 360 in Year ${draftCal.calendarYear})`} style={{ color: "#38bdf8", fontWeight: 600, whiteSpace: "nowrap" }}>{draftCal.calendarFormatted}</span>
      </div>
      <div style={{ display: "grid", gridTemplateColumns: "repeat(4, 1fr)", gap: "4px" }}>
        {VERRA_SEASON_PRESETS.map((s) => (
          <button key={s.id} type="button" title={s.tooltip} data-testid={`verra-season-preset-${s.id}`} onClick={() => onApplyPresetDate(s.dayOfYear, draftCal.calendarYear)} style={{ height: "24px", padding: "0 4px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontFamily: "inherit", fontSize: "10px", cursor: "pointer" }}>
            {s.label}
          </button>
        ))}
      </div>
      <div style={{ display: "grid", gridTemplateColumns: "74px 1fr 78px", gap: "6px", alignItems: "center" }}>
        <label title="Calendar year (1 or higher)" style={{ display: "flex", flexDirection: "column", gap: "2px", fontSize: "10px", whiteSpace: "nowrap", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>
          Year
          <input type="number" min={1} max={9999} aria-label="Verra Calendar Year" data-testid="verra-calendar-year-input" value={draftCal.calendarYear} onChange={(e) => onChangeDate(draftCal.calendarDayOfYear, Math.max(1, Number(e.currentTarget.value) || 1))} style={inputStyle} />
        </label>
        <label title="90-day Verra season" style={{ display: "flex", flexDirection: "column", gap: "2px", fontSize: "10px", whiteSpace: "nowrap", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>
          Season
          <select aria-label="Verra Calendar Season" data-testid="verra-calendar-season-select" value={draftCal.calendarSeasonIndex} onChange={(e) => onChangeDate((Math.max(1, Math.min(4, Number(e.currentTarget.value))) - 1) * 90 + draftCal.calendarDayOfSeason, draftCal.calendarYear)} style={inputStyle}>
            {VERRA_CALENDAR_SEASON_NAMES.map((sName, idx) => (<option key={sName} value={idx + 1} title={`Days ${idx * 90 + 1}-${(idx + 1) * 90}`}>{sName}</option>))}
          </select>
        </label>
        <label title="Day within current season (1 to 90)" style={{ display: "flex", flexDirection: "column", gap: "2px", fontSize: "10px", whiteSpace: "nowrap", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>
          Day
          <input type="number" min={1} max={90} aria-label="Verra Calendar Day of Season" data-testid="verra-calendar-day-of-season-input" value={draftCal.calendarDayOfSeason} onChange={(e) => onChangeDate((draftCal.calendarSeasonIndex - 1) * 90 + Math.max(1, Math.min(90, Number(e.currentTarget.value) || 1)), draftCal.calendarYear)} style={inputStyle} />
        </label>
      </div>
      <input type="range" min={1} max={360} step={1} title={`Day ${draftCal.calendarDayOfYear} of 360`} aria-label="Verra Calendar Day of Year (1-360)" data-testid="verra-calendar-day-slider" value={draftCal.calendarDayOfYear} onChange={(e) => onChangeDate(Number(e.currentTarget.value), draftCal.calendarYear)} style={{ width: "100%", height: "6px", cursor: "pointer" }} />
    </div>
  );
}

export { formatTimeOfDayLabel, parseTimeOfDayInput };
