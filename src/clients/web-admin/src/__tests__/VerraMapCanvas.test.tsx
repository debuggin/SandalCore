// SPDX-License-Identifier: AGPL-3.0-only

import { existsSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import {
  computeViewportWorldBoundsCm,
  computeVerraCalendar,
  createWorldStreamStore,
  DEFAULT_CAMERA_VIEWPORT,
  DEFAULT_REGIONAL_WORLD_WORKERS,
  formatCanonicalWorkerName,
  formatWorkerTickHzOptionLabel,
  getVisibleWorkersForViewport,
  loadPersistedWebAdminState,
  normalizeWorkerTickHz,
  savePersistedWebAdminState,
  SESSION_UI_STORAGE_KEY,
  type WorldWorkerInfo,
  WORKER_TICK_HZ_OPTIONS,
} from "../state/worldStreamStore";

function readVerraMapCanvasSource(): string {
  const canvasPath = fileURLToPath(new URL("../map/VerraMapCanvas.tsx", import.meta.url));
  const popoverPath = fileURLToPath(new URL("../map/VerraCalendarPopover.tsx", import.meta.url));
  return (
    readFileSync(canvasPath, "utf8") +
    (existsSync(popoverPath) ? "\n" + readFileSync(popoverPath, "utf8") : "")
  );
}

describe("VerraMapCanvas Wave 41E/41F Regression Suite", () => {
  it("exports WORKER_TICK_HZ_OPTIONS as [-1, 0, 10, 15, 20, 25, 30, 40, 50, 60] where -1 = Dynamic and 0 = 0 Hz Pause Worker NPCs", async () => {
    const mapCanvasSource = readVerraMapCanvasSource();
    expect(mapCanvasSource.includes("toggle-simulate-all-npcs")).toBe(false);
    expect([...WORKER_TICK_HZ_OPTIONS]).toEqual([-1, 0, 10, 15, 20, 25, 30, 40, 50, 60]);
    expect(normalizeWorkerTickHz(-1)).toBe(-1);
    expect(normalizeWorkerTickHz("dynamic")).toBe(-1);
    expect(normalizeWorkerTickHz(0)).toBe(0);
    expect(normalizeWorkerTickHz(60)).toBe(60);
    expect(formatWorkerTickHzOptionLabel(-1)).toContain("Dynamic");
    expect(formatWorkerTickHzOptionLabel(0)).toContain("0 Hz");
    expect(formatWorkerTickHzOptionLabel(30)).toBe("30 Hz");

    const store = createWorldStreamStore({ workers: DEFAULT_REGIONAL_WORLD_WORKERS });
    let capturedBody = "";
    await store.getState().executeSetWorkerTickRateAction("world-riverlands", 0, async (_input, init) => {
      capturedBody = String(init?.body ?? "");
      return new Response(JSON.stringify({ ok: true }), { status: 200 });
    });
    const parsed = JSON.parse(capturedBody);
    expect(parsed.tick_hz).toBe(0);
    expect(parsed.tick_mode).toBe("manual");
    expect(parsed.simulate_npcs).toBe(false);
    expect(store.getState().workers.find((w) => w.worker_id === "world-riverlands")?.simulate_npcs).toBe(false);
  });

  it("widens visible-server-workers-panel to 450px and displays only real configured workers (0 phantom 24-worker injection)", () => {
    const mapCanvasSource = readVerraMapCanvasSource();
    expect(mapCanvasSource).toContain('data-testid="visible-server-workers-panel"');
    expect(mapCanvasSource).toMatch(/450px/);
    expect(formatCanonicalWorkerName("zoi", "verra_rvr_node_winstead")).toBe("aoc-verra-rvr-node-winstead");

    const defaultBounds = computeViewportWorldBoundsCm(DEFAULT_CAMERA_VIEWPORT);
    const defaultVisible = getVisibleWorkersForViewport(DEFAULT_REGIONAL_WORLD_WORKERS, defaultBounds);
    expect(defaultVisible.length).toBeLessThanOrEqual(DEFAULT_REGIONAL_WORLD_WORKERS.length);
    expect(defaultVisible.every((w) => DEFAULT_REGIONAL_WORLD_WORKERS.some((c) => c.worker_id === w.worker_id))).toBe(true);

    const explicitSevenWorkers: readonly WorldWorkerInfo[] = [
      ...DEFAULT_REGIONAL_WORLD_WORKERS,
      {
        worker_id: "aoc-verra-rvr-node-winstead",
        canonical_name: "aoc-verra-rvr-node-winstead",
        scope_key: "verra_rvr_node_winstead",
        scope_kind: "zoi",
        region_label: "Riverlands \u00b7 Winstead ZOI",
        container_name: "aoc-verra-rvr-node-winstead",
        status: "ONLINE",
        tick_hz: 60,
        players_count: 1,
        npcs_count: 120,
      },
      {
        worker_id: "aoc-winstead-node-plug",
        canonical_name: "aoc-winstead",
        scope_key: "verra_rvr_node_winstead",
        scope_kind: "node_plug",
        region_label: "Riverlands \u00b7 Winstead Node Plug",
        container_name: "aoc-winstead",
        status: "ONLINE",
        tick_hz: 60,
        players_count: 1,
        npcs_count: 45,
      },
    ];
    const fullMapBounds = { minX: -2500000, minY: -2000000, maxX: 2500000, maxY: 2000000 };
    const visibleWithSeven = getVisibleWorkersForViewport(explicitSevenWorkers, fullMapBounds);
    expect(visibleWithSeven.length).toBe(7);
    const winsteadZoi = visibleWithSeven.find((w) => w.worker_id === "aoc-verra-rvr-node-winstead");
    expect(winsteadZoi).toBeDefined();
    expect(winsteadZoi?.region_label).toBe("Riverlands \u00b7 Winstead ZOI");
  });

  it("renders time-of-day-popover (zIndex: 1200) with Verra Calendar controls and prevents sessionStorage from overwriting backend time_of_day_hours", () => {
    const mapCanvasSource = readVerraMapCanvasSource();
    const store = createWorldStreamStore({
      timeOfDayHours: 18.5,
      calendarDayOfYear: 95,
      calendarYear: 2,
      calendarSeason: "Summer (Suncrest)",
    });
    expect(
      mapCanvasSource.includes('data-testid="time-of-day-popover"') ||
        mapCanvasSource.includes('data-testid="world-time-picker-popover"'),
    ).toBe(true);
    expect(mapCanvasSource).toMatch(/zIndex:\s*(?:[a-zA-Z0-9_]+\s*\?\s*)?1200\b/);
    expect(mapCanvasSource).toContain("verra-calendar-year-input");
    expect(mapCanvasSource).toContain("verra-calendar-day-slider");

    const cal = computeVerraCalendar(95, 2);
    expect(cal.calendarDayOfYear).toBe(95);
    expect(cal.calendarYear).toBe(2);
    expect(cal.calendarSeason).toBe("Summer (Suncrest)");

    const fakeStorage = new Map<string, string>();
    const storageAdapter = {
      getItem: (k: string) => fakeStorage.get(k) ?? null,
      setItem: (k: string, v: string) => {
        fakeStorage.set(k, v);
      },
    };
    savePersistedWebAdminState(store.getState(), storageAdapter);
    fakeStorage.set(
      SESSION_UI_STORAGE_KEY,
      JSON.stringify({
        ...JSON.parse(fakeStorage.get(SESSION_UI_STORAGE_KEY) ?? "{}"),
        timeOfDayHours: 3.25,
        timeOfDayLabel: "03:15",
      }),
    );

    store.getState().setTimeOfDay(18.5, 1700000000000);
    store.getState().restoreForAuthenticatedSession(storageAdapter);
    expect(store.getState().timeOfDayHours).toBe(18.5);
    expect(store.getState().timeOfDayLabel).toBe("18:30");

    const loaded = loadPersistedWebAdminState(storageAdapter, "", true);
    expect(loaded.timeOfDayHours).toBe(12.0);
  });
});
