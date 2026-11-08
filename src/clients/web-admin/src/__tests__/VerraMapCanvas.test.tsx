// SPDX-License-Identifier: AGPL-3.0-only

import { existsSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import {
  buildEntityInspectorModel,
  formatPoiCategoryDisplay,
  poiOverlayToInspectedEntity,
  resolveMutuallyExclusiveInspectorTarget,
} from "../components/EntityInspector";
import {
  classifyWorldEntityCategory,
  drawCanvasScene,
  findNearestEntityOrNodeAtScreen,
  mergePoiOverlays,
  parsePoisFromPayload,
  POI_FALLBACK_ICON_BY_KIND,
  resolveEntityMapMarkerColor,
  resolvePoiIconName,
  VerraTileCache,
  worldCmToScreenOffset,
} from "../map/verraTilePyramidRenderer";
import { parseWorldStateResponse } from "../map/VerraMapCanvas";
import { VERRA_SEASON_PRESETS } from "../map/VerraCalendarPopover";
import {
  computeViewportWorldBoundsCm,
  computeVerraCalendar,
  createWorldStreamStore,
  DEFAULT_CAMERA_VIEWPORT,
  DEFAULT_LAYER_TOGGLES,
  DEFAULT_REGIONAL_WORLD_WORKERS,
  DEFAULT_VERRA_MAP_META,
  formatCanonicalWorkerName,
  formatWorkerTickHzOptionLabel,
  getVisibleWorkersForViewport,
  inferResourceNodeType,
  loadPersistedWebAdminState,
  normalizeWorkerTickHz,
  resolveWorkerTickFields,
  savePersistedWebAdminState,
  SESSION_UI_STORAGE_KEY,
  VERRA_CALENDAR_SEASON_NAMES,
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

  it("renders time-of-day-popover (zIndex: 1200) with authentic 4-Season Verra Calendar controls, bottom-left vertical legend, and uniform StreamStatus typography", () => {
    const mapCanvasSource = readVerraMapCanvasSource();
    const streamStatusSource = readFileSync(fileURLToPath(new URL("../components/StreamStatus.tsx", import.meta.url)), "utf8");
    const storeSource = readFileSync(fileURLToPath(new URL("../state/worldStreamStore.ts", import.meta.url)), "utf8");
    const store = createWorldStreamStore({
      timeOfDayHours: 18.5, calendarDayOfYear: 95, calendarYear: 2, calendarSeason: "Summer",
    });
    expect(
      mapCanvasSource.includes('data-testid="time-of-day-popover"') ||
        mapCanvasSource.includes('data-testid="world-time-picker-popover"'),
    ).toBe(true);
    expect(mapCanvasSource).toMatch(/zIndex:\s*(?:[a-zA-Z0-9_]+\s*\?\s*)?1200\b/);
    expect(mapCanvasSource).toContain("verra-calendar-year-input");
    expect(mapCanvasSource).toContain("verra-calendar-season-select");
    expect(mapCanvasSource).toContain("verra-calendar-day-of-season-input");
    expect(mapCanvasSource).toContain("verra-calendar-day-slider");
    expect(mapCanvasSource.includes("Newbloom")).toBe(false);
    expect(mapCanvasSource.includes("Suncrest")).toBe(false);
    expect(storeSource.includes("Newbloom")).toBe(false);
    expect(storeSource.includes("Stonecutting")).toBe(false);
    expect([...VERRA_CALENDAR_SEASON_NAMES]).toEqual(["Spring", "Summer", "Autumn", "Winter"]);
    expect(inferResourceNodeType("Granite Rock")).toBe("Mining / Stone");

    expect(mapCanvasSource).toContain('left: "12px", bottom: "12px"');
    expect(mapCanvasSource).toContain('flexDirection: "column"');
    expect(mapCanvasSource.indexOf('data-testid="map-entity-legend"')).toBeGreaterThan(mapCanvasSource.indexOf("</header>"));

    expect(streamStatusSource).toContain('{`Players Online: ${counts.players}`}');
    expect(streamStatusSource.includes("tabular-nums")).toBe(false);

    const cal = computeVerraCalendar(95, 2);
    expect([cal.calendarDayOfYear, cal.calendarYear, cal.calendarSeason, cal.calendarSeasonIndex, cal.calendarDayOfSeason]).toEqual([95, 2, "Summer", 2, 5]);

    const fakeStorage = new Map<string, string>();
    const storageAdapter = {
      getItem: (k: string) => fakeStorage.get(k) ?? null,
      setItem: (k: string, v: string) => { fakeStorage.set(k, v); },
    };
    savePersistedWebAdminState(store.getState(), storageAdapter);
    fakeStorage.set(
      SESSION_UI_STORAGE_KEY,
      JSON.stringify({ ...JSON.parse(fakeStorage.get(SESSION_UI_STORAGE_KEY) ?? "{}"), timeOfDayHours: 3.25, timeOfDayLabel: "03:15" }),
    );

    store.getState().setTimeOfDay(18.5, 1700000000000);
    store.getState().restoreForAuthenticatedSession(storageAdapter);
    expect(store.getState().timeOfDayHours).toBe(18.5);
    expect(store.getState().timeOfDayLabel).toBe("18:30");

    const loaded = loadPersistedWebAdminState(storageAdapter, "", true);
    expect(loaded.timeOfDayHours).toBe(12.0);
  });

  it("separates target_tick_hz (dropdown setting) from current_tick_hz (live output) so Dynamic (-1) never snaps back to a number on poll", async () => {
    const store = createWorldStreamStore({ workers: DEFAULT_REGIONAL_WORLD_WORKERS });
    const initialWorker = store.getState().workers.find((w) => w.worker_id === "world-riverlands")!;
    expect(initialWorker.target_tick_hz).toBe(-1);
    expect(initialWorker.tick_mode).toBe("dynamic");
    expect(initialWorker.current_tick_hz).toBeGreaterThanOrEqual(30);

    await store.getState().executeSetWorkerTickRateAction("world-riverlands", 20, async () => new Response(JSON.stringify({ ok: true }), { status: 200 }));
    expect(store.getState().workers.find((w) => w.worker_id === "world-riverlands")?.target_tick_hz).toBe(20);
    expect(store.getState().workers.find((w) => w.worker_id === "world-riverlands")?.current_tick_hz).toBe(20);

    await store.getState().executeSetWorkerTickRateAction("world-riverlands", -1, async () => new Response(JSON.stringify({ ok: true }), { status: 200 }));
    store.getState().setWorkers([
      {
        ...initialWorker,
        worker_id: "world-riverlands",
        tick_hz: 45,
        current_tick_hz: 45,
        target_tick_hz: -1,
        tick_mode: "dynamic",
      },
    ]);
    const afterPoll = resolveWorkerTickFields(store.getState().workers[0]);
    expect(afterPoll.target_tick_hz).toBe(-1);
    expect(afterPoll.tick_mode).toBe("dynamic");
    expect(afterPoll.current_tick_hz).toBe(45);
  });

  it("hit-tests and selects Ember Springs and all POI types on the map canvas and populates authentic metadata in EntityInspector", () => {
    const viewport = { width: 960, height: 540 };
    const parsedPois = parsePoisFromPayload({
      places: [
        {
          guid: "attune-lionhold-01", name: "Attune_RVR_Lionhold_01", display_name: "Lionhold Ember Spring",
          category: "wayshrine", kind: "wayshrine", node_name: "Winstead", node_type: "Economic", biome: "Riverlands",
          x: -777664, y: 522023, z: 11524, radius: 5000, level: 10, description: "Ancient attunable respawn shrine",
        },
        {
          guid: "poi-carphin-01", name: "POI_RVR_TowerOfCarphin", display_name: "Tower of Carphin",
          category: "dungeon", kind: "dungeon", node_name: "Winstead", biome: "Riverlands",
          x: -720000, y: 500000, z: 14200, radius: 12000, min_level: 20, max_level: 30,
          description: "Corrupted spire dungeon", tags: ["Dungeon", "Elite"],
        },
      ],
    });
    expect(parsedPois.length).toBe(2);
    expect(formatPoiCategoryDisplay(parsedPois[0])).toBe("Ember Spring");
    expect(formatPoiCategoryDisplay(parsedPois[1])).toBe("Dungeon");
    expect(mergePoiOverlays(parsedPois, [parsedPois[0]]).length).toBe(2);

    const store = createWorldStreamStore();
    store.getState().setPois(parsedPois);
    store.getState().applyWorldDelta({
      seq: 1, isSnapshot: true,
      added: [{ guid: "npc-nearby-guard", name: "Lionhold Guard", kind: "npc", xCm: -777664 + 1200, yCm: 522023 + 1200, zCm: 11524, yawDeg: 0, level: 25, healthPct: 100 }],
    });

    const [esX, esY] = worldCmToScreenOffset(parsedPois[0].x_cm, parsedPois[0].y_cm, viewport, store.getState().camera);
    const hitEmberSpring = findNearestEntityOrNodeAtScreen(esX, esY, viewport, store.getState());
    expect(hitEmberSpring?.poi?.guid).toBe("attune-lionhold-01");

    store.getState().selectPoi(hitEmberSpring!.poi!.guid);
    const excl = resolveMutuallyExclusiveInspectorTarget(store.getState());
    expect([excl.activeKind, excl.entity?.name, excl.entity?.poiCategory, excl.entity?.recordName]).toEqual(["poi", "Lionhold Ember Spring", "Ember Spring", "Attune_RVR_Lionhold_01"]);

    const emberModel = buildEntityInspectorModel({ entity: excl.entity });
    expect(emberModel.rows.find((r) => r.rowId === "category")?.value).toBe("Ember Spring");
    expect(emberModel.rows.find((r) => r.rowId === "record_name")?.value).toBe("Attune_RVR_Lionhold_01");
    expect(emberModel.rows.find((r) => r.rowId === "region_zoi")?.value).toContain("Riverlands");
    expect(emberModel.rows.find((r) => r.rowId === "radius")?.value).toBe("50.0 m (5000 cm)");
    expect(emberModel.rows.find((r) => r.rowId === "node_association")?.value).toContain("Winstead");
    expect(emberModel.rows.find((r) => r.rowId === "description")?.value).toContain("Ancient attunable respawn shrine");

    const [poiX, poiY] = worldCmToScreenOffset(parsedPois[1].x_cm, parsedPois[1].y_cm, viewport, store.getState().camera);
    const hitDungeon = findNearestEntityOrNodeAtScreen(poiX, poiY, viewport, store.getState());
    expect(hitDungeon?.poi?.guid).toBe("poi-carphin-01");
    const dungeonModel = buildEntityInspectorModel({ entity: poiOverlayToInspectedEntity(hitDungeon!.poi!, store.getState().workers) });
    expect(dungeonModel.rows.find((r) => r.rowId === "category")?.value).toBe("Dungeon");
    expect(dungeonModel.rows.find((r) => r.rowId === "level")?.value).toBe("20-30");
    expect(dungeonModel.rows.find((r) => r.rowId === "description")?.value).toContain("Corrupted spire dungeon");
  });

  it("classifies bare Verra resource def_names into distinct marker colors and renders clean VerraCalendarPopover labels and dark inputs", () => {
    const bareResourceCases = [
      { defName: "Tree_EasternHemlock_Large_1", category: "trees", color: "#22c55e" },
      { defName: "Stone_Basalt_Medium_1", category: "rocks", color: "#94a3b8" },
      { defName: "Stone_Wyrdstone_Large_1", category: "ore_gems", color: "#eab308" },
      { defName: "Stone_Resonite_Medium_1", category: "ore_gems", color: "#eab308" },
      { defName: "Metal_Rividium_Large_1", category: "ore_gems", color: "#eab308" },
      { defName: "Plant_Banana_Fruited_1", category: "plants", color: "#2dd4bf" },
      { defName: "Coral_BlueCoral_Large_1", category: "plants", color: "#2dd4bf" },
    ] as const;

    for (const c of bareResourceCases) {
      const entity = { kind: "resource_node" as const, name: c.defName, resource_type: undefined, resourceType: undefined };
      expect(classifyWorldEntityCategory(entity)).toBe(c.category);
      expect(resolveEntityMapMarkerColor(entity)).toBe(c.color);
    }

    expect(VERRA_SEASON_PRESETS.map((p) => p.label)).toEqual(["Spring", "Summer", "Autumn", "Winter"]);
    expect(VERRA_SEASON_PRESETS.some((p) => p.label.includes("(D"))).toBe(false);

    const popoverSrc = readFileSync(fileURLToPath(new URL("../map/VerraCalendarPopover.tsx", import.meta.url)), "utf8");
    expect(popoverSrc.includes("(D1)")).toBe(false);
    expect(popoverSrc).toContain('colorScheme: "dark"');
    expect(popoverSrc).toContain("Verra Calendar");
    expect(popoverSrc.includes("Verra Calendar (360d)")).toBe(false);

    const rendererSrc = readFileSync(fileURLToPath(new URL("../map/verraTilePyramidRenderer.ts", import.meta.url)), "utf8");
    const drawSceneBlock = rendererSrc.slice(
      rendererSrc.indexOf("export function drawCanvasScene"),
      rendererSrc.indexOf("function drawZoiBorderOverlays"),
    );
    const entityOrderIdx = drawSceneBlock.indexOf("drawEntityAndCrosshairMarkers(");
    const poiOrderIdx = drawSceneBlock.indexOf("drawPoiAndCityNodeMarkers(");
    expect(entityOrderIdx).toBeGreaterThan(0);
    expect(poiOrderIdx).toBeGreaterThan(entityOrderIdx);

    const labelBlock = rendererSrc.slice(
      rendererSrc.indexOf("function drawCollisionSafeLabel"),
      rendererSrc.indexOf("export type WorldPopulationCategory"),
    );
    expect(labelBlock).toMatch(/#F6CC7E/i);
    expect(labelBlock).toContain("strokeText");
  });

  it("draws POI and City Node labels centered above icons without NODE_STAGE_NAMES / (Wilderness) suffixes and omits Move Speed strip from TeleportCard", () => {
    const rendererSrc = readFileSync(fileURLToPath(new URL("../map/verraTilePyramidRenderer.ts", import.meta.url)), "utf8");
    const markerBlock = rendererSrc.slice(
      rendererSrc.indexOf("function drawPoiAndCityNodeMarkers"),
      rendererSrc.indexOf("export type WorldPopulationCategory"),
    );
    expect(markerBlock.includes("NODE_STAGE_NAMES")).toBe(false);
    expect(markerBlock).toContain("sy - 21");
    expect(markerBlock).toContain("sy - Math.round(iconPx / 2) - 7");
    expect(markerBlock).toContain("Wilderness|Expedition|Encampment|Village|Town|City|Metropolis");

    const drawnTexts: Array<{ text: string; x: number; y: number }> = [];
    const mockCtx = {
      fillStyle: "", strokeStyle: "", lineWidth: 1, font: "", textBaseline: "alphabetic", lineJoin: "miter",
      fillRect: () => {}, strokeRect: () => {}, clip: () => {}, drawImage: () => {},
      beginPath: () => {}, closePath: () => {}, setLineDash: () => {}, arc: () => {},
      fill: () => {}, stroke: () => {}, moveTo: () => {}, lineTo: () => {}, rect: () => {},
      strokeText: () => {}, fillText: (text: string, x: number, y: number) => { drawnTexts.push({ text, x, y }); },
      measureText: (txt: string) => ({ width: txt.length * 8 }),
      save: () => {}, restore: () => {}, translate: () => {}, rotate: () => {},
    } as unknown as CanvasRenderingContext2D;
    const store = createWorldStreamStore();
    store.getState().setCameraViewport({ centerXCm: -644519, centerYCm: 376773, zoom: 3.5 });
    store.getState().setPois([{ guid: "poi-test-1", name: "POI_Test", display_name: "Carphin Spire (Wilderness)", category: "dungeon", kind: "dungeon", x_cm: -610000, y_cm: 376773, z_cm: 11000, icon_name: "T_Icon", icon_url: "/api/v1/world/icons/T_Icon.png" }]);
    drawCanvasScene(mockCtx, { width: 960, height: 540 }, DEFAULT_VERRA_MAP_META, store.getState(), new VerraTileCache(() => {}));
    expect(drawnTexts.some((d) => /\((?:Wilderness|Expedition|Village|Town|City|Metropolis)\)/i.test(d.text))).toBe(false);
    expect(drawnTexts.some((d) => d.text === "Winstead")).toBe(true);
    expect(drawnTexts.some((d) => d.text === "Carphin Spire")).toBe(true);

    const playersTabSrc = readFileSync(fileURLToPath(new URL("../components/PlayersAndSpawnerTab.tsx", import.meta.url)), "utf8");
    expect(playersTabSrc).toContain("Teleport Player ({activeChar || \"Select Player\"})");
    expect(playersTabSrc.includes("Teleport & Movement Speed")).toBe(false);
    expect(playersTabSrc.includes("Move Speed:")).toBe(false);
  });

  it("maps POI fallback icons, filters offline/duplicate players in parseWorldStateResponse, and isolates per-worker tick rate overrides across shared container_name", async () => {
    expect(POI_FALLBACK_ICON_BY_KIND.harbor).toBe("TUI_Icon_Map_Node_Dock");
    expect(POI_FALLBACK_ICON_BY_KIND.castle).toBe("TUI_Icon_Map_Siege");
    expect(POI_FALLBACK_ICON_BY_KIND.starting_area).toBe("TUI_Icon_Map_HomeShrine");
    expect(POI_FALLBACK_ICON_BY_KIND.spawn).toBe("TUI_Icon_Map_HomeShrine");
    expect(POI_FALLBACK_ICON_BY_KIND.outpost).toBe("TUI_Icon_Map_MinorPOI_Discovered");
    expect(POI_FALLBACK_ICON_BY_KIND.city_node).toBe("TUI_Icon_Node_LVL3");
    expect(POI_FALLBACK_ICON_BY_KIND.node_plug).toBe("TUI_Icon_Node_LVL3");
    expect(resolvePoiIconName("harbor")).toBe("TUI_Icon_Map_Node_Dock");
    expect(resolvePoiIconName("castle")).toBe("TUI_Icon_Map_Siege");
    expect(resolvePoiIconName("starting_area")).toBe("TUI_Icon_Map_HomeShrine");
    expect(resolvePoiIconName("spawn")).toBe("TUI_Icon_Map_HomeShrine");
    expect(resolvePoiIconName("outpost")).toBe("TUI_Icon_Map_MinorPOI_Discovered");
    expect(resolvePoiIconName("city_node")).toBe("TUI_Icon_Node_LVL3");
    expect(resolvePoiIconName("node_plug")).toBe("TUI_Icon_Node_LVL3");

    const parsedEntities = parseWorldStateResponse(
      {
        players: [
          { character_id: "char-1", name: "Aurelia", online: true, x_cm: -640000, y_cm: 376000, z_cm: 12000 },
          { character_id: "char-2", name: "OfflineHero", online: false, x_cm: -640100, y_cm: 376100, z_cm: 12000 },
          { character_id: "char-3", name: "StateOffline", state: "offline", x_cm: -640200, y_cm: 376200, z_cm: 12000 },
        ],
        entities: [
          { id: "char-1", name: "Aurelia", kind: "player", online: true, x_cm: -640000, y_cm: 376000, z_cm: 12000 },
          { id: "char-99", name: "aurelia", kind: "player", online: true, x_cm: -640000, y_cm: 376000, z_cm: 12000 },
          { id: "char-4", name: "OfflineEnt", kind: "player", online: false, x_cm: -640300, y_cm: 376300, z_cm: 12000 },
          { id: "char-5", name: "Kaelen", kind: "player", online: true, x_cm: -641000, y_cm: 377000, z_cm: 12000 },
        ],
      },
      DEFAULT_LAYER_TOGGLES,
    );
    const playerEntities = parsedEntities.filter((e) => e.kind === "player");
    expect(playerEntities.map((p) => p.name)).toEqual(["Aurelia", "Kaelen"]);

    const sharedContainerWorkers: readonly WorldWorkerInfo[] = DEFAULT_REGIONAL_WORLD_WORKERS.map((w) => ({
      ...w,
      container_name: "aoc-world",
      target_tick_hz: -1,
      tick_mode: "dynamic",
    }));
    const store = createWorldStreamStore({ workers: sharedContainerWorkers });
    await store.getState().executeSetWorkerTickRateAction("world-riverlands", 25, async () => new Response(JSON.stringify({ ok: true }), { status: 200 }));
    const riverlands = store.getState().workers.find((w) => w.worker_id === "world-riverlands")!;
    const anvils = store.getState().workers.find((w) => w.worker_id === "world-anvils")!;
    expect(riverlands.target_tick_hz).toBe(25);
    expect(riverlands.tick_mode).toBe("manual");
    expect(anvils.target_tick_hz).toBe(-1);
    expect(anvils.tick_mode).toBe("dynamic");
    await store.getState().executeSetWorkerTickRateAction("world-riverlands", -1, async () => new Response(JSON.stringify({ ok: true }), { status: 200 }));
  });
});
