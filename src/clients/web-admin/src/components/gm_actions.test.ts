// SPDX-License-Identifier: AGPL-3.0-only

import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { registerHooks } from "node:module";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

// Registers in-memory Node module hooks so .tsx modules strip JSX return trees without DOM.
registerHooks({
  resolve(specifier, context, nextResolve) {
    if (specifier === "react") {
      return {
        url: "data:text/javascript,export const useState = (v) => [typeof v === 'function' ? v() : v, () => {}]; export const useEffect = () => {}; export const useMemo = (fn) => fn(); export const useCallback = (fn) => fn; export const useRef = (v) => ({ current: v }); export const createElement = () => null; export const StrictMode = 'StrictMode'; export class Component { constructor(props) { this.props = props; this.state = {}; } setState() {} } export default { useState, useEffect, useMemo, useCallback, useRef, createElement, StrictMode, Component };",
        shortCircuit: true,
      };
    }
    if (specifier === "react-dom" || specifier === "react-dom/client") {
      return {
        url: "data:text/javascript,export const createRoot = () => ({ render: () => {} }); export const createPortal = (el) => el; export default { createRoot, createPortal };",
        shortCircuit: true,
      };
    }
    if (specifier.startsWith(".") && context.parentURL?.startsWith("file:")) {
      const baseDir = dirname(fileURLToPath(context.parentURL));
      for (const ext of ["", ".ts", ".tsx"]) {
        const candidate = resolve(baseDir, `${specifier}${ext}`);
        if (existsSync(candidate)) {
          return { url: pathToFileURL(candidate).href, shortCircuit: true };
        }
      }
    }
    return nextResolve(specifier, context);
  },
  load(url, context, nextLoad) {
    if (url.endsWith(".tsx")) {
      const raw = readFileSync(fileURLToPath(url), "utf8");
      const stripped = raw
        .replace(
          /return\s*\(\s*<([A-Za-z0-9_]+)[\s\S]*?<\/\1>\s*\);/g,
          "return null as any;",
        )
        .replace(
          /return\s*\(\s*<[A-Za-z0-9_]+[\s\S]*?\/>\s*\);/g,
          "return null as any;",
        )
        .replace(/return\s+<([A-Za-z0-9_]+)[^>]*\/>;/g, "return null as any;");
      return nextLoad(url, {
        ...context,
        format: "module-typescript",
        source: stripped,
      });
    }
    return nextLoad(url, context);
  },
});

async function testVerraCoordinateMathAndDynamicTilePyramid(): Promise<void> {
  const storeMod = await import("../state/worldStreamStore.ts");
  assert.equal(storeMod.DEFAULT_VERRA_MAP_META.x0, -47);
  assert.equal(storeMod.DEFAULT_VERRA_MAP_META.y0, -17);
  assert.equal(storeMod.DEFAULT_VERRA_MAP_META.cols, 51);
  assert.equal(storeMod.DEFAULT_VERRA_MAP_META.rows, 46);
  assert.equal(storeMod.DEFAULT_VERRA_MAP_META.source_cm, 50000);
  assert.equal(storeMod.DEFAULT_VERRA_MAP_META.source_px, 512);
  assert.equal(storeMod.DEFAULT_VERRA_MAP_META.tile_px, 256);
  assert.equal(storeMod.DEFAULT_VERRA_MAP_META.width_px, 26112);
  assert.equal(storeMod.DEFAULT_VERRA_MAP_META.height_px, 23552);
  assert.equal(storeMod.DEFAULT_VERRA_MAP_META.max_zoom, 7);
  assert.equal(storeMod.DEFAULT_VERRA_MAP_META.plug_zoom, 8);

  const [originU, originV] = storeMod.worldCmToMosaicPx(0, 0);
  assert.equal(originU, 47.5 * 512);
  assert.equal(originV, 17.5 * 512);
  const [roundX, roundY] = storeMod.mosaicPxToWorldCm(originU, originV);
  assert.equal(roundX, 0);
  assert.equal(roundY, 0);

  const z0Tiles = storeMod.computeVisibleBaseTiles(
    storeMod.DEFAULT_VERRA_MAP_META,
    -777664,
    522023,
    -2.0,
    { width: 1280, height: 720 },
  );
  assert.ok(z0Tiles.length > 0 && z0Tiles.every((t) => t.z === 0));
  const z1Tiles = storeMod.computeVisibleBaseTiles(
    storeMod.DEFAULT_VERRA_MAP_META,
    -777664,
    522023,
    0.0,
    { width: 1280, height: 720 },
  );
  assert.ok(z1Tiles.length > 0 && z1Tiles.every((t) => t.z === 1));
  const z3Tiles = storeMod.computeVisibleBaseTiles(
    storeMod.DEFAULT_VERRA_MAP_META,
    -777664,
    522023,
    2.0,
    { width: 1280, height: 720 },
  );
  assert.ok(z3Tiles.length > 0 && z3Tiles.every((t) => t.z === 3));
  const z6Tiles = storeMod.computeVisibleBaseTiles(
    storeMod.DEFAULT_VERRA_MAP_META,
    -777664,
    522023,
    5.0,
    { width: 1280, height: 720 },
  );
  assert.ok(z6Tiles.length > 0 && z6Tiles.every((t) => t.z === 6));
  const z7Tiles = storeMod.computeVisibleBaseTiles(
    storeMod.DEFAULT_VERRA_MAP_META,
    -777664,
    522023,
    8.0,
    { width: 1280, height: 720 },
  );
  assert.ok(z7Tiles.length > 0 && z7Tiles.every((t) => t.z === 7));
  assert.ok(
    (z6Tiles[0].distFromCenterPx ?? 0) <=
      (z6Tiles[z6Tiles.length - 1].distFromCenterPx ?? 0),
  );
}

async function testNodePlugTransformZoiPolygonsAndPureBlackCanvasRendering(): Promise<void> {
  const storeMod = await import("../state/worldStreamStore.ts");
  const mapMod = await import("../map/VerraMapCanvas.tsx");

  assert.equal(storeMod.DEFAULT_VERRA_CITY_NODES.length, 26);
  const winstead = storeMod.DEFAULT_VERRA_CITY_NODES[0];
  assert.equal(winstead.ox, -644519.11);
  assert.equal(winstead.oy, 376773.24);
  assert.equal(winstead.yaw_deg, 319.20846);

  // Level 0 (Wilderness) renders L0 plug (plugTier = 0) instead of null
  const level0Overlay = storeMod.computeNodePlugTileOverlay({
    ...winstead,
    level: 0,
  });
  assert.ok(level0Overlay !== null);
  assert.equal(level0Overlay.plugTier, 0);
  assert.equal(level0Overlay.tileUrl, "/api/v1/world/tiles/8/0/0");

  // Universal +25,000 cm (+0.5 tile) offset in worldCmToMosaicPx aligns mobs, NPCs, POIs & CityNodes
  const center = storeMod.getCityNodeCenterCm(winstead);
  assert.equal(center.cx_cm, winstead.ox);
  assert.equal(center.cy_cm, winstead.oy);

  const defaultStage3 = storeMod.computeNodePlugTileOverlay(
    { ...winstead, level: 3 },
    5.0,
  );
  assert.ok(defaultStage3 !== null);
  assert.equal(defaultStage3.plugTier, 2);
  assert.equal(defaultStage3.tileUrl, "/api/v1/world/tiles/8/0/2");
  assert.equal(defaultStage3.cxCm, center.cx_cm);
  assert.equal(defaultStage3.cyCm, center.cy_cm);
  assert.equal(defaultStage3.worldSpanCm, 100000);
  assert.equal(defaultStage3.drawSpanPx, 1024);
  assert.ok(
    Math.abs(defaultStage3.rotationRad - (winstead.yaw_deg * Math.PI) / 180.0) <
      1e-6,
  );

  // Verify 55 ZOI Polygons, in-game CityNodeRecordBase.BannerColor hex values, and Emberspring icons
  assert.equal(mapMod.VERRA_ZOI_POLYGONS.length, 55);
  assert.equal(mapMod.VERRA_ZOI_POLYGONS[0].display_name, "Miraleth");
  assert.equal(mapMod.VERRA_ZOI_POLYGONS[0].color, "#49629F");
  assert.equal(mapMod.VERRA_ZOI_POLYGONS[1].color, "#BF5858");
  assert.equal(mapMod.VERRA_ZOI_POLYGONS[5].color, "#E8B25A");
  assert.equal(storeMod.DEFAULT_LAYER_TOGGLES.zoiBorders, true);
  assert.equal(mapMod.resolvePoiIconName("emberspring"), "TUI_Icon_Map_ReviveShrine_Active");
  assert.equal(mapMod.resolvePoiIconName("wayshrine"), "TUI_Icon_Map_ReviveShrine_Active");
  const parsedEntities = mapMod.parseWorldStateResponse(
    { entities: [{ id: "es-1", name: "Emberspring", kind: "npc", npc_subkind: "emberspring", x_cm: -644519, y_cm: 376773, z_cm: 12000 }] },
    storeMod.DEFAULT_LAYER_TOGGLES,
  );
  assert.equal(parsedEntities[0]?.npc_subkind, "emberspring");

  // Verify pure #000000 black background and 0 wireframe strokeRect calls when tiles are missing
  const fillStyles: string[] = [];
  let strokeRectCalls = 0;
  let clipCalls = 0;
  const mockCtx = {
    set fillStyle(val: string) {
      fillStyles.push(val);
    },
    fillRect: () => {},
    strokeRect: () => {
      strokeRectCalls += 1;
    },
    clip: () => {
      clipCalls += 1;
    },
    drawImage: () => {},
    beginPath: () => {},
    closePath: () => {},
    setLineDash: () => {},
    arc: () => {},
    fill: () => {},
    stroke: () => {},
    moveTo: () => {},
    lineTo: () => {},
    rect: () => {},
    fillText: () => {},
    measureText: (txt: string) => ({ width: txt.length * 6 }),
    save: () => {},
    restore: () => {},
    translate: () => {},
    rotate: () => {},
  } as unknown as CanvasRenderingContext2D;

  const store = storeMod.createWorldStreamStore();
  const cache = new mapMod.VerraTileCache(() => {});
  assert.equal(cache.maxInflight, 16);
  mapMod.drawCanvasScene(
    mockCtx,
    { width: 960, height: 540 },
    storeMod.DEFAULT_VERRA_MAP_META,
    store.getState(),
    cache,
  );
  assert.equal(fillStyles[0], "#000000");
  assert.equal(strokeRectCalls, 0);
  assert.equal(clipCalls, 0);
}

async function testRightClickClearScopedReceiptsMayorElectionsAndPersistence(): Promise<void> {
  const storeMod = await import("../state/worldStreamStore.ts");
  const mapMod = await import("../map/VerraMapCanvas.tsx");
  const tab2Mod = await import("./PlayersAndSpawnerTab.tsx");
  const tab3Mod = await import("./LogsAndContainersTab.tsx");
  const mainMod = await import("../main.tsx");

  // Verify unified Analytics & Logs screen (2 top-level tabs: Map, Analytics & Logs)
  assert.deepEqual(
    mainMod.ADMIN_TABS.map((t) => ({ id: t.id, label: t.label })),
    [
      { id: "map", label: "Map" },
      { id: "analytics", label: "Analytics & Logs" },
    ],
  );
  assert.equal(typeof mainMod.WebAdminErrorBoundary, "function");
  assert.equal(typeof tab2Mod.TeleportCard, "function");
  assert.equal(typeof tab2Mod.GiveGoldAndItemCards, "function");
  assert.equal(typeof tab2Mod.SpawnNpcCard, "function");
  assert.equal(typeof tab2Mod.NodeGovernanceModalCard, "function");

  const store = storeMod.createWorldStreamStore();
  store
    .getState()
    .setPickedCoordinate({ xCm: -641389, yCm: 377367, zCm: 12500 });
  store.getState().selectCityNode("Verra_RVR_Winstead");
  assert.equal(store.getState().selectedCityNodeName, "Verra_RVR_Winstead");
  assert.equal(store.getState().selectedEntityGuid, null);
  store.getState().selectEntity("ent-100");
  assert.equal(store.getState().selectedEntityGuid, "ent-100");
  assert.equal(store.getState().selectedCityNodeName, null);
  store.getState().selectPoi("poi-222");
  assert.equal(store.getState().selectedPoiGuid, "poi-222");
  assert.equal(store.getState().selectedEntityGuid, null);
  assert.equal(store.getState().selectedCityNodeName, null);
  assert.ok(store.getState().lastClickedPoint !== null);

  let simCmdSent = false;
  await store.getState().setSimulateAllNpcs(false, async () => {
    simCmdSent = true;
    return new Response(JSON.stringify({ ok: true }), { status: 200 });
  });
  assert.equal(store.getState().simulateAllNpcs, false);
  assert.equal(simCmdSent, true);

  mapMod.handleCanvasRightClickClear(store.getState());
  assert.equal(store.getState().lastClickedPoint, null);
  assert.equal(store.getState().pickedCoordinate, null);
  assert.equal(store.getState().selectedEntityGuid, null);
  assert.equal(store.getState().selectedCityNodeName, null);
  assert.equal(store.getState().selectedPoiGuid, null);

  // Scoped command receipts never bleed across controls
  store
    .getState()
    .setScopedReceipt("nodeLevel", {
      ok: true,
      message: "Windansea -> Lv 6: EXECUTED",
    });
  assert.equal(
    store.getState().lastReceiptsByScope.nodeLevel?.message,
    "Windansea -> Lv 6: EXECUTED",
  );
  assert.equal(store.getState().lastReceiptsByScope.teleport, null);
  assert.equal(store.getState().lastReceiptsByScope.moveSpeed, null);
  assert.equal(store.getState().lastReceiptsByScope.spawnNpc, null);
  assert.equal(store.getState().lastReceiptsByScope.giveItem, null);
  assert.equal(store.getState().lastReceiptsByScope.currency, null);
  assert.equal(store.getState().lastReceiptsByScope.nodeMayor, null);
  assert.equal(store.getState().lastReceiptsByScope.nodeElection, null);
  assert.equal(store.getState().lastReceiptsByScope.nodeXp, null);
  assert.equal(store.getState().lastReceiptsByScope.entityAction, null);

  // Mayor & Election actions
  const mockFetch: typeof fetch = async (input) => {
    const url = String(input);
    if (url.includes("/mayor")) {
      return new Response(JSON.stringify({ message: "Mayor updated to Cyb" }), {
        status: 200,
      });
    }
    return new Response(
      JSON.stringify({ election_status: "voting", message: "Voting opened" }),
      { status: 200 },
    );
  };
  await store
    .getState()
    .setNodeMayor("Verra_RVR_Winstead", "Cyb", mockFetch);
  assert.equal(
    store
      .getState()
      .cityNodes.find((n) => n.name === "Verra_RVR_Winstead")?.mayor_name,
    "Cyb",
  );
  assert.equal(
    store.getState().lastReceiptsByScope.nodeMayor?.message,
    "Mayor updated to Cyb",
  );
  assert.equal(store.getState().lastReceiptsByScope.teleport, null);

  await store
    .getState()
    .triggerNodeElection("Verra_RVR_Winstead", "open_voting", mockFetch);
  assert.equal(
    store
      .getState()
      .cityNodes.find((n) => n.name === "Verra_RVR_Winstead")?.election_status,
    "voting",
  );
  assert.equal(
    store.getState().lastReceiptsByScope.nodeElection?.message,
    "Voting opened",
  );

  // Prometheus & container scrape normalization
  const emptyScrape = tab3Mod.normalizeContainersScrapeResponse({});
  assert.equal(emptyScrape.world_status.players_online, 0);
  assert.equal(emptyScrape.world_status.npcs_total, 0);
  assert.equal(emptyScrape.prometheus.targets.length, 3);

  // Persistence round-trip for both activeTab and activeModal (localStorage + URL hash)
  const memStorage = new Map<string, string>();
  const storageStub = {
    getItem: (k: string) => memStorage.get(k) ?? null,
    setItem: (k: string, v: string) => {
      memStorage.set(k, v);
    },
  };
  store.getState().setActiveTab("map");
  store.getState().setActiveModal("spawn-npc");
  store.getState().setSelectedPlayer("Aurelia");
  store.getState().setTeleportMode("to_player");
  store.getState().setSpawnTargetMode("coords");
  store.getState().setSelectedContainer("aoc-gateway");
  store.getState().setLogQuery("WARN");
  storeMod.savePersistedWebAdminState(store.getState(), storageStub);
  const restored = storeMod.loadPersistedWebAdminState(
    storageStub,
    "#tab=analytics&modal=node-governance",
  );
  assert.equal(restored.activeTab, "analytics");
  assert.equal(restored.activeModal, "node-governance");
  assert.equal(restored.selectedPlayer, "Aurelia");
  assert.equal(restored.teleportMode, "to_player");
  assert.equal(restored.spawnTargetMode, "coords");
  assert.equal(restored.selectedContainer, "aoc-gateway");
  assert.equal(restored.logQuery, "WARN");
}

async function testStreamLayerBitmasksPoisAndFindInWorldSearch(): Promise<void> {
  const storeMod = await import("../state/worldStreamStore.ts");
  const mapMod = await import("../map/VerraMapCanvas.tsx");

  const defaultLayers = {
    players: true,
    npcs: true,
    monsters: true,
    resource_nodes: false,
    mounts: false,
    loot_piles: false,
    pois: true,
  };
  assert.equal(storeMod.computeStreamLayerMask(defaultLayers), 99);
  assert.equal(
    storeMod.buildWorldStatePollUrl(defaultLayers),
    "/api/v1/world/state?layer_mask=99&layers=players,npcs,monsters,pois",
  );

  const parsedPois = mapMod.parsePoisFromPayload({
    pois: [
      {
        guid: "poi-1",
        name: "Wayshrine_Lionhold",
        display_name: "Lionhold Wayshrine",
        category: "wayshrine",
        x_cm: -450000,
        y_cm: 280000,
        z_cm: 11000,
        icon_name: "T_Icon_Wayshrine",
      },
    ],
  });
  assert.equal(parsedPois.length, 1);
  assert.equal(
    parsedPois[0].icon_url,
    "/api/v1/world/icons/TUI_Icon_Map_ReviveShrine_Active.png",
  );

  const entities = new Map([
    [
      "mob-1",
      {
        guid: "mob-1",
        name: "Ashen Grizzly",
        kind: "monster" as const,
        xCm: 100,
        yCm: 200,
        zCm: 0,
        yawDeg: 0,
        level: 22,
        healthPct: 100,
      },
    ],
  ]);
  const counts = storeMod.computeLayerEntityCounts({
    entities,
    pois: parsedPois,
    cityNodes: storeMod.DEFAULT_VERRA_CITY_NODES,
  });
  assert.equal(mapMod.formatOverlayLayerLabel("monsters", counts), "Monsters (1)");
  assert.equal(mapMod.formatOverlayLayerLabel("pois", counts), "POIs (27)");

  const hits = mapMod.filterMapSearchItems(
    "grizzly",
    storeMod.DEFAULT_VERRA_CITY_NODES,
    entities,
    [],
    parsedPois,
  );
  assert.equal(hits.length, 1);
  assert.equal(hits[0].entityGuid, "mob-1");
}

async function testGmCommandBuildersPaginationAndTransientRetry(): Promise<void> {
  const paletteMod = await import("./GmCommandPalette.tsx");
  const menuMod = await import("./WorldContextMenu.tsx");
  const inspectorMod = await import("./EntityInspector.tsx");
  const storeMod = await import("../state/worldStreamStore.ts");
  const logsMod = await import("./LogsAndContainersTab.tsx");

  const rvrWorker = storeMod.resolveWorkerForCoordinateOrZone(-644519, 376773);
  assert.equal(rvrWorker.worker_id, "world-riverlands");
  assert.equal(rvrWorker.container_name, "aoc-world-riverlands");

  const sqdWorker = storeMod.resolveWorkerForCoordinateOrZone(-278767, 987391);
  assert.equal(sqdWorker.worker_id, "world-sandsquall");
  assert.equal(sqdWorker.container_name, "aoc-world-sandsquall");

  const jndWorker = storeMod.resolveWorkerForCoordinateOrZone(-1708500, 1053019);
  assert.equal(jndWorker.worker_id, "world-jundark");

  const anvWorker = storeMod.resolveWorkerForCoordinateOrZone(-1069784, -150970);
  assert.equal(anvWorker.worker_id, "world-anvils");

  const trqWorker = storeMod.resolveWorkerForCoordinateOrZone(-1860980, -171832);
  assert.equal(trqWorker.worker_id, "world-turquoise-sea");

  const zoiWorker = storeMod.resolveWorkerForCoordinateOrZone(0, 0, "zoi:Verra_ZOI_Miraleth");
  assert.equal(zoiWorker.scope_kind, "zoi");
  assert.equal(zoiWorker.worker_id, "world-zoi-verra-zoi-miraleth");

  const nodeWorker = storeMod.resolveWorkerForCoordinateOrZone(0, 0, "node:Winstead");
  assert.equal(nodeWorker.scope_kind, "node");
  assert.equal(nodeWorker.worker_id, "world-node-winstead");

  const snap = logsMod.normalizeContainersScrapeResponse({
    workers: [
      {
        worker_id: "world-node-winstead",
        scope_kind: "node",
        scope_key: "node:Winstead",
        region_label: "Winstead",
        container_name: "aoc-world-node-winstead",
        status: "UP",
        tick_hz: 30,
        npcs: 180,
        monsters: 20,
        resource_nodes: 45,
        active_players: 2,
      },
    ],
  });
  assert.equal(snap.workers.length, 6);
  assert.equal(snap.workers[0].worker_id, "world-riverlands");
  assert.equal(snap.workers[5].worker_id, "world-node-winstead");

  const tpPlace = paletteMod.buildTeleportPlayerCommand({
    character: "Aurelia",
    place: "Lionhold",
  });
  assert.equal(tpPlace.command, "teleport_player");
  const moneyCmd = paletteMod.buildGiveMoneyCommand("Aurelia", 10, 25, 50);
  assert.equal(moneyCmd.gold, 10);
  const itemCmd = paletteMod.buildGrantItemCommand(
    "Aurelia",
    "60000000001",
    5,
    "Legendary",
  );
  assert.equal(itemCmd.rarity, "Legendary");
  const spawnCmd = paletteMod.buildSpawnNpcCommand({
    npc: "40000000009",
    level: 35,
    x_cm: 1000,
    y_cm: 2000,
    z_cm: 300,
  });
  assert.equal(spawnCmd.level, 35);
  const nodeCmd = paletteMod.buildSetNodeLevelCommand("Winstead", 4);
  assert.equal(nodeCmd.plug_tier, 3);

  let attempts = 0;
  const mockFetch: typeof fetch = async () => {
    attempts += 1;
    if (attempts === 1) {
      return new Response(JSON.stringify({ title: "Unavailable" }), {
        status: 503,
      });
    }
    return new Response(JSON.stringify({ message: "Node stage updated" }), {
      status: 200,
    });
  };
  const cmdRes = await paletteMod.sendGmCommand(nodeCmd, mockFetch);
  assert.equal(cmdRes.ok, true);
  assert.equal(attempts, 2);

  assert.equal(paletteMod.validateGmCommandRbac("GM_OBSERVER", "Teleport"), false);
  assert.equal(paletteMod.validateGmCommandRbac("GM_OPERATOR", "Teleport"), true);
  assert.equal(
    menuMod.buildGroundSnappedTeleportPayload(
      { xCm: 10, yCm: 20, zCm: 30 },
      "c-1",
    ).capsule_half_height_cm,
    96.0,
  );
  assert.equal(inspectorMod.moveInspectorGridRowFocus(0, 9, "ArrowDown"), 1);
}

await testVerraCoordinateMathAndDynamicTilePyramid();
await testNodePlugTransformZoiPolygonsAndPureBlackCanvasRendering();
await testRightClickClearScopedReceiptsMayorElectionsAndPersistence();
await testStreamLayerBitmasksPoisAndFindInWorldSearch();
await testGmCommandBuildersPaginationAndTransientRetry();
console.log("PASS: gm_actions unit and layer bitmask contract suite (5/5 groups)");
