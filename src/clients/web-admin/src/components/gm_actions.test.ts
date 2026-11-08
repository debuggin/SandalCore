// SPDX-License-Identifier: AGPL-3.0-only

import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { registerHooks } from "node:module";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

// Registers in-memory Node module hooks so .tsx modules strip JSX return trees without DOM.
if (!process.env.VITEST) registerHooks({
  resolve(specifier, context, nextResolve) {
    if (specifier === "react" || specifier === "react/jsx-runtime" || specifier === "react/jsx-dev-runtime") {
      return {
        url: "data:text/javascript,export const useState = (v) => [typeof v === 'function' ? v() : v, () => {}]; export const useEffect = () => {}; export const useMemo = (fn) => fn(); export const useCallback = (fn) => fn; export const useRef = (v) => ({ current: v }); export const createElement = () => null; export const jsx = () => null; export const jsxs = () => null; export const jsxDEV = () => null; export const Fragment = 'Fragment'; export const StrictMode = 'StrictMode'; export class Component { constructor(props) { this.props = props; this.state = {}; } setState() {} } export default { useState, useEffect, useMemo, useCallback, useRef, createElement, jsx, jsxs, jsxDEV, Fragment, StrictMode, Component };",
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
          /^([ \t]+)return\s*\(\s*<[A-Za-z0-9_]+[\s\S]*?^\1\);/gm,
          "$1return null as any;",
        )
        .replace(
          /return\s*\(\s*<([A-Za-z0-9_]+)[\s\S]*?<\/\1>\s*\);/g,
          "return null as any;",
        )
        .replace(
          /return\s*\(\s*<[A-Za-z0-9_]+[\s\S]*?\/>\s*\);/g,
          "return null as any;",
        )
        .replace(/return\s+<([A-Za-z0-9_]+)[\s\S]*?\/>;/g, "return null as any;");
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

  const vp = { width: 1280, height: 720 };
  const z0Tiles = storeMod.computeVisibleBaseTiles(storeMod.DEFAULT_VERRA_MAP_META, -777664, 522023, -3.0, vp);
  const z1Tiles = storeMod.computeVisibleBaseTiles(storeMod.DEFAULT_VERRA_MAP_META, -777664, 522023, -2.0, vp);
  const z3Tiles = storeMod.computeVisibleBaseTiles(storeMod.DEFAULT_VERRA_MAP_META, -777664, 522023, 0.0, vp);
  const z6Tiles = storeMod.computeVisibleBaseTiles(storeMod.DEFAULT_VERRA_MAP_META, -777664, 522023, 2.5, vp);
  const z7Tiles = storeMod.computeVisibleBaseTiles(storeMod.DEFAULT_VERRA_MAP_META, -777664, 522023, 3.5, vp);
  assert.ok(z0Tiles.length > 0 && z0Tiles.every((t) => t.z === 0));
  assert.ok(z1Tiles.length > 0 && z1Tiles.every((t) => t.z === 1));
  assert.ok(z3Tiles.length > 0 && z3Tiles.every((t) => t.z === 3));
  assert.ok(z6Tiles.length > 0 && z6Tiles.every((t) => t.z === 6));
  assert.ok(z7Tiles.length > 0 && z7Tiles.every((t) => t.z === 7));
  assert.ok((z7Tiles[0].distFromCenterPx ?? 0) <= (z7Tiles[z7Tiles.length - 1].distFromCenterPx ?? 0));
  assert.equal(storeMod.STREAM_TARGET_HZ, 30);
  assert.equal(storeMod.getMapPollHzForZoom(0), 30);
  assert.equal(storeMod.getMapPollHzForZoom(7), 30);
  assert.equal(storeMod.getMapPollIntervalMsForZoom(2), 33);
}

async function testNodePlugTransformAndAuthenticCityNodeStageIcons(): Promise<void> {
  const storeMod = await import("../state/worldStreamStore.ts");
  const mapMod = await import("../map/VerraMapCanvas.tsx");

  assert.equal(storeMod.DEFAULT_VERRA_CITY_NODES.length, 26);
  const winstead = storeMod.DEFAULT_VERRA_CITY_NODES[0];
  assert.equal(winstead.ox, -644519.11);
  assert.equal(winstead.oy, 376773.24);
  assert.equal(winstead.yaw_deg, 319.20846);

  const level0Overlay = storeMod.computeNodePlugTileOverlay({ ...winstead, level: 0 });
  assert.ok(level0Overlay !== null);
  assert.equal(level0Overlay.plugTier, 0);
  assert.equal(level0Overlay.tileUrl, "/api/v1/world/tiles/8/0/0");

  const center = storeMod.getCityNodeCenterCm(winstead);
  assert.equal(center.cx_cm, winstead.ox);
  assert.equal(center.cy_cm, winstead.oy);

  const defaultStage3 = storeMod.computeNodePlugTileOverlay({ ...winstead, level: 3 }, 5.0);
  assert.ok(defaultStage3 !== null);
  assert.equal(defaultStage3.plugTier, 2);
  assert.equal(defaultStage3.tileUrl, "/api/v1/world/tiles/8/0/2");
  assert.equal(defaultStage3.cxCm, center.cx_cm);
  assert.equal(defaultStage3.cyCm, center.cy_cm);
  assert.equal(defaultStage3.worldSpanCm, 100000);
  assert.equal(defaultStage3.drawSpanPx, 1024);
  assert.ok(Math.abs(defaultStage3.rotationRad - (winstead.yaw_deg * Math.PI) / 180.0) < 1e-6);

  // Verify resolveCityNodeIconName(0..6) authentic stage icons
  assert.equal(mapMod.resolveCityNodeIconName({ level: 0 }), "TUI_Icon_Node_LVL0");
  assert.equal(mapMod.resolveCityNodeIconName({ level: 1 }), "TUI_Icon_Node_LVL0");
  assert.equal(mapMod.resolveCityNodeIconName({ level: 2 }), "TUI_Icon_Node_LVL1");
  assert.equal(mapMod.resolveCityNodeIconName({ level: 3 }), "TUI_Icon_Node_LVL2");
  assert.equal(mapMod.resolveCityNodeIconName({ level: 4 }), "TUI_Icon_Node_LVL3");
  assert.equal(mapMod.resolveCityNodeIconName({ level: 5 }), "TUI_Icon_Node_LVL4");
  assert.equal(mapMod.resolveCityNodeIconName({ level: 6 }), "TUI_Icon_Node_Sovereignty");
}

async function testZoiPolygonsVhalgadimClippingAndBlackCanvas(): Promise<void> {
  const storeMod = await import("../state/worldStreamStore.ts");
  const mapMod = await import("../map/VerraMapCanvas.tsx");

  assert.equal(mapMod.VERRA_ZOI_POLYGONS.length, 55);
  assert.equal(mapMod.VERRA_ZOI_POLYGONS.filter((z) => z.has_in_game_border).length, 27);
  assert.equal(mapMod.VERRA_ZOI_POLYGONS.filter((z) => !z.has_in_game_border).length, 28);
  assert.equal(mapMod.VERRA_ZOI_POLYGONS.find((z) => z.zoi_index === 49)?.has_in_game_border, true);
  assert.equal(mapMod.VERRA_ZOI_POLYGONS.find((z) => z.zoi_index === 36)?.has_in_game_border, false);

  // Verify ZOI #44 (Verra_ANV_Node_Vhalgadim) is clipped to valid non-black tile boundary (u <= 17408 where v <= 8704)
  const vhalgadim = mapMod.VERRA_ZOI_POLYGONS.find((z) => z.zoi_index === 44);
  assert.ok(vhalgadim !== undefined);
  assert.equal(vhalgadim.name, "Verra_ANV_Node_Vhalgadim");
  assert.equal(vhalgadim.has_in_game_border, true);
  assert.ok(vhalgadim.centroid_u <= 17408);
  for (const ring of vhalgadim.rings) {
    for (const [u, v] of ring) {
      if (v <= 8704) assert.ok(u <= 17408, `Expected Vhalgadim vertex u=${u} <= 17408 at v=${v}`);
    }
  }

  const fillStyles: string[] = [];
  let strokeRectCalls = 0;
  let clipCalls = 0;
  let arcCallsBeforeFirstClip = 0;
  let arcCalls = 0;
  const mockCtx = {
    set fillStyle(val: string) { fillStyles.push(val); },
    fillRect: () => {},
    strokeRect: () => { strokeRectCalls += 1; },
    clip: () => { if (clipCalls === 0) arcCallsBeforeFirstClip = arcCalls; clipCalls += 1; },
    drawImage: () => {}, beginPath: () => {}, closePath: () => {}, setLineDash: () => {},
    arc: () => { arcCalls += 1; }, fill: () => {}, stroke: () => {}, moveTo: () => {}, lineTo: () => {},
    rect: () => {}, fillText: () => {}, measureText: (txt: string) => ({ width: txt.length * 6 }),
    save: () => {}, restore: () => {}, translate: () => {}, rotate: () => {},
  } as unknown as CanvasRenderingContext2D;

  const store = storeMod.createWorldStreamStore();
  const cache = new mapMod.VerraTileCache(() => {});
  assert.equal(cache.maxInflight, 32);
  mapMod.drawCanvasScene(mockCtx, { width: 960, height: 540 }, storeMod.DEFAULT_VERRA_MAP_META, store.getState(), cache);
  assert.equal(fillStyles[0], "#000000");
  assert.equal(strokeRectCalls, 0);
  assert.ok(clipCalls >= 27);
  assert.equal(arcCallsBeforeFirstClip, 0);
}

async function testMutuallyExclusiveSelectionAndRightClickClear(): Promise<void> {
  const storeMod = await import("../state/worldStreamStore.ts");
  const mapMod = await import("../map/VerraMapCanvas.tsx");
  const tab2Mod = await import("./PlayersAndSpawnerTab.tsx");
  const mainMod = await import("../main.tsx");

  assert.deepEqual(
    mainMod.ADMIN_TABS.map((t) => ({ id: t.id, label: t.label })),
    [{ id: "map", label: "Map" }, { id: "analytics", label: "Observability" }],
  );
  assert.equal(typeof mainMod.WebAdminErrorBoundary, "function");
  assert.equal(typeof tab2Mod.TeleportCard, "function");
  assert.equal(typeof tab2Mod.GiveGoldAndItemCards, "function");
  assert.equal(typeof tab2Mod.SpawnNpcCard, "function");
  assert.equal(typeof tab2Mod.NodeGovernanceModalCard, "function");

  const store = storeMod.createWorldStreamStore();
  store.getState().setPickedCoordinate({ xCm: -641389, yCm: 377367, zCm: 12500 });
  assert.ok(store.getState().pickedCoordinate !== null);
  store.getState().selectCityNode("Verra_RVR_Winstead");
  assert.equal(store.getState().selectedCityNodeName, "Verra_RVR_Winstead");
  assert.equal(store.getState().selectedEntityGuid, null);
  assert.equal(store.getState().pickedCoordinate, null);
  store.getState().selectEntity("ent-100");
  assert.equal(store.getState().selectedEntityGuid, "ent-100");
  assert.equal(store.getState().selectedCityNodeName, null);
  assert.equal(store.getState().pickedCoordinate, null);
  store.getState().selectPoi("poi-222");
  assert.equal(store.getState().selectedPoiGuid, "poi-222");
  assert.equal(store.getState().selectedEntityGuid, null);
  assert.equal(store.getState().selectedCityNodeName, null);
  assert.equal(store.getState().lastClickedPoint, null);
  store.getState().setPickedCoordinate({ xCm: -641389, yCm: 377367, zCm: 12500 });
  assert.equal(store.getState().selectedPoiGuid, null);
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
}

async function testScopedReceiptsMayorElectionsAndPersistence(): Promise<void> {
  const storeMod = await import("../state/worldStreamStore.ts");
  const store = storeMod.createWorldStreamStore();

  store.getState().setScopedReceipt("nodeLevel", { ok: true, message: "Windansea -> Lv 6: EXECUTED" });
  assert.equal(store.getState().lastReceiptsByScope.nodeLevel?.message, "Windansea -> Lv 6: EXECUTED");
  assert.equal(store.getState().lastReceiptsByScope.teleport, null);
  assert.equal(store.getState().lastReceiptsByScope.spawnNpc, null);
  assert.equal(store.getState().lastReceiptsByScope.nodeMayor, null);
  assert.equal(store.getState().lastReceiptsByScope.nodeElection, null);
  assert.equal(store.getState().lastReceiptsByScope.nodeXp, null);

  const mockFetch: typeof fetch = async (input) => {
    const url = String(input);
    if (url.includes("/mayor")) {
      return new Response(JSON.stringify({ message: "Mayor updated to Cyb" }), { status: 200 });
    }
    return new Response(JSON.stringify({ election_status: "voting", message: "Voting opened" }), { status: 200 });
  };
  await store.getState().setNodeMayor("Verra_RVR_Winstead", "Cyb", mockFetch);
  assert.equal(store.getState().cityNodes.find((n) => n.name === "Verra_RVR_Winstead")?.mayor_name, "Cyb");
  assert.equal(store.getState().lastReceiptsByScope.nodeMayor?.message, "Mayor updated to Cyb");
  assert.equal(store.getState().lastReceiptsByScope.teleport, null);

  await store.getState().triggerNodeElection("Verra_RVR_Winstead", "open_voting", mockFetch);
  assert.equal(store.getState().cityNodes.find((n) => n.name === "Verra_RVR_Winstead")?.election_status, "voting");
  assert.equal(store.getState().lastReceiptsByScope.nodeElection?.message, "Voting opened");

  const memStorage = new Map<string, string>();
  const storageStub = {
    getItem: (k: string) => memStorage.get(k) ?? null,
    setItem: (k: string, v: string) => { memStorage.set(k, v); },
  };
  store.getState().setActiveTab("map");
  store.getState().setActiveModal("spawn-npc");
  store.getState().setSelectedPlayer("Aurelia");
  store.getState().setTeleportMode("player");
  store.getState().setSpawnTargetMode("coords");
  store.getState().setSelectedContainer("aoc-gateway");
  store.getState().setLogQuery("WARN");
  storeMod.savePersistedWebAdminState(store.getState(), storageStub);
  const restored = storeMod.loadPersistedWebAdminState(storageStub, "#tab=analytics&modal=node-governance");
  assert.equal(restored.activeTab, "analytics");
  assert.equal(restored.activeModal, "node-governance");
  assert.equal(restored.selectedPlayer, "Aurelia");
  assert.equal(restored.teleportMode, "player");
  assert.equal(restored.spawnTargetMode, "coords");
  assert.equal(restored.selectedContainer, "aoc-gateway");
  assert.equal(restored.logQuery, "WARN");
}

async function testStreamLayerBitmasksPoisAndFindInWorldSearch(): Promise<void> {
  const storeMod = await import("../state/worldStreamStore.ts");
  const mapMod = await import("../map/VerraMapCanvas.tsx");

  const defaultLayers = {
    players: true, npcs: true, monsters: true, resource_nodes: false, mounts: false, loot_piles: false, pois: true,
  };
  assert.equal(storeMod.computeStreamLayerMask(defaultLayers), 99);
  assert.equal(
    storeMod.buildWorldStatePollUrl(defaultLayers),
    "/api/v1/world/state?layer_mask=99&layers=players,npcs,monsters,pois",
  );

  const parsedPois = mapMod.parsePoisFromPayload({
    pois: [{
      guid: "poi-1", name: "Wayshrine_Lionhold", display_name: "Lionhold Wayshrine",
      category: "wayshrine", x_cm: -450000, y_cm: 280000, z_cm: 11000, icon_name: "T_Icon_Wayshrine",
    }],
  });
  assert.equal(parsedPois.length, 1);
  assert.equal(parsedPois[0].icon_url, "/api/v1/world/icons/TUI_Icon_Map_ReviveShrine_Active.png");

  const entities = new Map([
    ["mob-1", { guid: "mob-1", name: "Ashen Grizzly", kind: "monster" as const, xCm: 100, yCm: 200, zCm: 0, yawDeg: 0, level: 22, healthPct: 100 }],
  ]);
  const counts = storeMod.computeLayerEntityCounts({ entities, pois: parsedPois, cityNodes: storeMod.DEFAULT_VERRA_CITY_NODES });
  assert.equal(mapMod.formatOverlayLayerLabel("monsters", counts), "Monsters (1)");
  assert.equal(mapMod.formatOverlayLayerLabel("pois", counts), "POIs (27)");

  const hits = mapMod.filterMapSearchItems("grizzly", storeMod.DEFAULT_VERRA_CITY_NODES, entities, [], parsedPois);
  assert.equal(hits.length, 1);
  assert.equal(hits[0].entityGuid, "mob-1");
}

async function testWorkerAuthorityResolutionAndCanonicalNames(): Promise<void> {
  const storeMod = await import("../state/worldStreamStore.ts");
  const logsMod = await import("./LogsAndContainersTab.tsx");

  const rvrWorker = storeMod.resolveWorkerForCoordinateOrZone(-644519, 376773);
  assert.equal(rvrWorker.worker_id, "world-riverlands");
  assert.equal(rvrWorker.container_name, "aoc-world-riverlands");

  const sqdWorker = storeMod.resolveWorkerForCoordinateOrZone(-278767, 987391);
  assert.equal(sqdWorker.worker_id, "world-sandsquall");
  assert.equal(sqdWorker.container_name, "aoc-world-sandsquall");
  assert.equal(storeMod.resolveWorkerForCoordinateOrZone(-1708500, 1053019).worker_id, "world-jundark");
  assert.equal(storeMod.resolveWorkerForCoordinateOrZone(-1069784, -150970).worker_id, "world-anvils");
  assert.equal(storeMod.resolveWorkerForCoordinateOrZone(-1860980, -171832).worker_id, "world-turquoise-sea");

  const zoiWorker = storeMod.resolveWorkerForCoordinateOrZone(0, 0, "zoi:Verra_ZOI_Miraleth");
  assert.equal(zoiWorker.scope_kind, "zoi");
  assert.equal(zoiWorker.worker_id, "world-zoi-verra-zoi-miraleth");
  const nodeWorker = storeMod.resolveWorkerForCoordinateOrZone(0, 0, "node:Winstead");
  assert.equal(nodeWorker.scope_kind, "node");
  assert.equal(nodeWorker.worker_id, "world-node-winstead");

  assert.equal(logsMod.formatCanonicalWorkerName("world", "verra"), "aoc-world");
  assert.equal(logsMod.formatCanonicalWorkerName("region", "riverlands"), "aoc-riverlands");
  assert.equal(logsMod.formatCanonicalWorkerName("node_plug", "node:Winstead"), "aoc-winstead");
  assert.equal(logsMod.formatCanonicalWorkerName("zoi", "Miraleth"), "aoc-miraleth");
  assert.equal(logsMod.formatCanonicalWorkerName("polygon_cutout", "poly:CitadelSiege"), "aoc-poly-citadelsiege");
}

async function testGmCommandBuildersPaginationAndTransientRetry(): Promise<void> {
  const paletteMod = await import("./GmCommandPalette.tsx");
  const menuMod = await import("./WorldContextMenu.tsx");
  const inspectorMod = await import("./EntityInspector.tsx");

  const tpPlace = paletteMod.buildTeleportPlayerCommand({ character: "Aurelia", place: "Lionhold" });
  assert.equal(tpPlace.command, "teleport_player");
  assert.equal(paletteMod.buildGiveMoneyCommand("Aurelia", 10, 25, 50).gold, 10);
  assert.equal(paletteMod.buildGrantItemCommand("Aurelia", "60000000001", 5, "Legendary").rarity, "Legendary");
  assert.equal(paletteMod.buildSpawnNpcCommand({ npc: "40000000009", level: 35, x_cm: 1000, y_cm: 2000, z_cm: 300 }).level, 35);
  const nodeCmd = paletteMod.buildSetNodeLevelCommand("Winstead", 4);
  assert.equal(nodeCmd.plug_tier, 3);

  let attempts = 0;
  const mockFetch: typeof fetch = async () => {
    attempts += 1;
    if (attempts === 1) return new Response(JSON.stringify({ title: "Unavailable" }), { status: 503 });
    return new Response(JSON.stringify({ message: "Node stage updated" }), { status: 200 });
  };
  const cmdRes = await paletteMod.sendGmCommand(nodeCmd, mockFetch);
  assert.equal(cmdRes.ok, true);
  assert.equal(attempts, 2);

  assert.equal(paletteMod.validateGmCommandRbac("GM_OBSERVER", "Teleport"), false);
  assert.equal(paletteMod.validateGmCommandRbac("GM_OPERATOR", "Teleport"), true);
  assert.equal(menuMod.buildGroundSnappedTeleportPayload({ xCm: 10, yCm: 20, zCm: 30 }, "c-1").capsule_half_height_cm, 96.0);
  assert.equal(inspectorMod.moveInspectorGridRowFocus(0, 9, "ArrowDown"), 1);
}

async function testInspectorModalActionsCatalogNpcsAndNpcRemoval(): Promise<void> {
  const storeMod = await import("../state/worldStreamStore.ts");
  const inspectorMod = await import("./EntityInspector.tsx");
  const modalsMod = await import("./NodeGovernanceAndSpawnModals.tsx");
  const tab2Mod = await import("./PlayersAndSpawnerTab.tsx");
  const modalsSrc = readFileSync(fileURLToPath(new URL("./NodeGovernanceAndSpawnModals.tsx", import.meta.url)), "utf8");
  const spawnerSrc = readFileSync(fileURLToPath(new URL("./PlayersAndSpawnerTab.tsx", import.meta.url)), "utf8");

  assert.equal(modalsSrc.includes("Spawn NPC\n        </button>"), true);
  assert.equal(modalsSrc.includes("Spawn {selectedNpc}") || modalsSrc.includes("Spawn Aela"), false);
  assert.equal(modalsSrc.includes("No matching places found"), true);
  assert.equal(modalsSrc.includes("e.stopPropagation()"), true);
  assert.equal(spawnerSrc.includes('placeholder="Search named place (e.g. Winstead, Lionshold, Carphin)..."'), true);
  assert.equal(spawnerSrc.includes("mouseDownOnBackdropRef"), true);
  assert.equal(tab2Mod.shouldUpdatePaginatedResourceState("places", { items: [], total: 0, limit: 25, offset: 0, has_more: false, next_cursor: null }, ""), true);
  assert.equal(tab2Mod.shouldCloseModalOnEscape({ key: "Escape", target: { tagName: "INPUT" } }), false);
  assert.equal(tab2Mod.shouldCloseModalOnEscape({ key: "Escape", target: { tagName: "DIV" } }, { tagName: "BODY" }), true);
  const mergedPlaces = tab2Mod.mergeTeleportCatalogPlaces(modalsMod.DEFAULT_CATALOG_PLACES, [{ guid: "poi-carphin", name: "POI_Carphin", display_name: "Tower of Carphin", category: "dungeon", x_cm: -720000, y_cm: 500000, z_cm: 14200, icon_name: "", icon_url: "" }], storeMod.DEFAULT_VERRA_CITY_NODES);
  const placeOpts = mergedPlaces.map((pl) => ({ value: pl.name, label: pl.display_name, detail: pl.name }));
  assert.ok(modalsMod.filterCatalogPickerOptions(placeOpts, "Carphin").some((o) => o.label === "Tower of Carphin"));
  assert.ok(modalsMod.filterCatalogPickerOptions(placeOpts, "Lionshold").some((o) => o.label === "Lionhold"));
  assert.ok(modalsMod.filterCatalogPickerOptions(placeOpts, "Winstead").some((o) => o.label === "Winstead"));
  assert.deepEqual(modalsMod.DEFAULT_CATALOG_NPCS.map((n) => ({ guid: n.guid, name: n.name, category: n.category })), [
    { guid: "6064628876645793792", name: "Aela", category: "npc" },
    { guid: "6064628931065511936", name: "Riverlands Wolf", category: "monster" },
    { guid: "6064628955249868800", name: "Citizen", category: "npc" },
  ]);

  const store = storeMod.createWorldStreamStore();
  store.getState().applyWorldDelta({ seq: 1, isSnapshot: true, added: [
    { guid: "npc-1", name: "Aela", kind: "npc", xCm: -644519.4, yCm: 376773.2, zCm: 12012.8, yawDeg: 0, level: 15, healthPct: 100 },
    { guid: "npc-2", name: "Riverlands Wolf", kind: "monster", xCm: -640000.0, yCm: 375000.0, zCm: 11800.0, yawDeg: 0, level: 12, healthPct: 100 },
  ] });
  store.getState().selectEntity("npc-1");
  const exclEntity = inspectorMod.resolveMutuallyExclusiveInspectorTarget(store.getState());
  assert.equal(exclEntity.activeKind, "entity");
  assert.equal(exclEntity.entity?.guid, "npc-1");
  assert.equal(exclEntity.pickedPosition, null);

  inspectorMod.openTeleportModalFromEntity(exclEntity.entity!, store.getState());
  assert.equal(store.getState().activeModal, "teleport");
  assert.equal(store.getState().teleportMode, "coords");
  assert.deepEqual(store.getState().pickedCoordinate, { xCm: -644519, yCm: 376773, zCm: 12013 });

  inspectorMod.openSpawnNpcModalFromEntity(exclEntity.entity!, store.getState());
  assert.equal(store.getState().activeModal, "spawn-npc");
  assert.equal(store.getState().spawnTargetMode, "coords");
  assert.deepEqual(store.getState().pickedCoordinate, { xCm: -644519, yCm: 376773, zCm: 12013 });

  let worldStateRefreshes = 0;
  const mockFetch: typeof fetch = async (input) => {
    if (String(input).includes("/api/v1/world/state")) { worldStateRefreshes += 1; return new Response(JSON.stringify({ entities: [] }), { status: 200 }); }
    return new Response(JSON.stringify({ ok: true, message: "executed" }), { status: 200 });
  };
  await modalsMod.dispatchAuxiliaryGmAction({ action: "kill_npc", entity_guid: "npc-1" }, mockFetch, store.getState());
  assert.equal(store.getState().entities.has("npc-1"), false);
  await modalsMod.dispatchAuxiliaryGmAction({ action: "despawn_npc", entity_guid: "npc-2" }, mockFetch, store.getState());
  assert.equal(store.getState().entities.has("npc-2"), false);
  assert.equal(worldStateRefreshes, 2);

  const errRes = await modalsMod.dispatchAuxiliaryGmAction({ action: "kill_npc", entity_guid: "missing" }, async () => new Response(JSON.stringify({ detail: "Entity not found in zone" }), { status: 404 }), store.getState());
  assert.equal(errRes.ok, false);
  assert.equal(errRes.message, "Entity not found in zone");
}

async function testScopedJwtAdminLoginAnd401Or403ReLogin(): Promise<void> {
  const authMod = await import("../state/adminAuthClient.ts");
  const loginModalMod = await import("./AdminLoginModal.tsx");
  const paletteMod = await import("./GmCommandPalette.tsx");

  assert.equal(typeof loginModalMod.AdminLoginModal, "function");
  const loginModalSrc = readFileSync(fileURLToPath(new URL("./AdminLoginModal.tsx", import.meta.url)), "utf8");
  assert.equal(loginModalSrc.includes("Enter the ephemeral Admin passphrase printed in the server startup console."), true);
  assert.equal(loginModalSrc.includes('"Start Session"'), true);
  assert.equal(loginModalSrc.includes('"End Session"'), true);
  assert.equal(loginModalSrc.includes("Requested Session Scopes:"), false);
  assert.equal(loginModalSrc.includes("Unlock World Admin Session"), false);
  authMod.clearAdminSession();
  assert.equal(authMod.getAdminAuthState().token, null);

  let loginPostedUrl = "";
  let loginPostedBody = "";
  const loginRes = await authMod.loginWithPassphrase("admin", "verra-secret-passphrase", async (input, init) => {
    loginPostedUrl = String(input);
    loginPostedBody = String(init?.body ?? "");
    return new Response(
      JSON.stringify({
        token: "hs256.jwt.token",
        username: "admin",
        role: "GM_ADMIN",
        scopes: ["admin:read", "admin:write"],
        boot_id: "boot-2026-40f",
      }),
      { status: 200 },
    );
  });
  assert.equal(loginRes.ok, true);
  assert.equal(loginPostedUrl, "/api/v1/admin/auth/login");
  assert.deepEqual(JSON.parse(loginPostedBody), { username: "admin", passphrase: "verra-secret-passphrase" });
  assert.equal(authMod.getAdminAuthState().token, "hs256.jwt.token");
  assert.equal(authMod.getAdminAuthState().bootId, "boot-2026-40f");
  assert.equal(authMod.getAdminAuthState().loginPromptOpen, false);

  const headers = authMod.buildAdminAuthHeaders({ "X-Custom": "1" });
  assert.equal(headers.Authorization, "Bearer hs256.jwt.token");
  assert.equal(headers["X-Custom"], "1");
  assert.equal(
    authMod.appendAdminAccessTokenToUrl("/api/v1/world/tiles/3/2/1"),
    "/api/v1/world/tiles/3/2/1?access_token=hs256.jwt.token",
  );

  // Verify 401 (boot_id mismatch on server restart) and 403 (missing scope) trigger AdminLoginModal
  await paletteMod.fetchWithTransientRetry(
    "/api/v1/gm/commands",
    { method: "POST" },
    { fetchImpl: async () => new Response(JSON.stringify({ detail: "boot_id mismatch" }), { status: 401 }) },
  );
  assert.equal(authMod.getAdminAuthState().authRequired, true);
  assert.equal(authMod.getAdminAuthState().loginPromptOpen, true);
  assert.equal(authMod.getAdminAuthState().token, null);

  authMod.handleUnauthorizedStatus(403, "Forbidden: missing admin:write scope");
  assert.equal(authMod.getAdminAuthState().loginPromptOpen, true);
  assert.equal(authMod.getAdminAuthState().lastAuthError, "Forbidden: missing admin:write scope");
}

async function testZeroObjectLogFalsificationAndConsistentEntityTaxonomy(): Promise<void> {
  const paletteMod = await import("./GmCommandPalette.tsx");
  const logsMod = await import("./LogsAndContainersTab.tsx");

  const logPayload = {
    items: [
      { ts: "2026-11-07T09:00:01Z", timestamp: "2026-11-07T09:00:01Z", stream: "stdout", container: "aoc-world-riverlands", message: '{"time":"2026-11-07T09:00:01Z","level":"INFO","component":"world","msg":"NPC looks ready","zone":"riverlands"}' },
      { ts: "2026-11-07T09:00:02Z", timestamp: "2026-11-07T09:00:02Z", stream: "stderr", container: "aoc-mesh-gateway", message: "[WARN] mesh retry budget 1/3" },
    ],
    lines: [
      '2026-11-07T09:00:01Z [stdout] {"time":"2026-11-07T09:00:01Z","level":"INFO","component":"world","msg":"NPC looks ready","zone":"riverlands"}',
      "2026-11-07T09:00:02Z [stderr] [WARN] mesh retry budget 1/3",
    ],
  };

  const paletteEnv = paletteMod.normalizePaginatedEnvelope<string>(logPayload, ["lines", "items"]);
  assert.equal(paletteEnv.items.length, 2);
  assert.equal(paletteEnv.items.some((line) => String(line).includes("[object Object]")), false);
  const itemsOnlyEnv = paletteMod.normalizePaginatedEnvelope<string>({ items: logPayload.items }, ["lines", "items"]);
  assert.equal(itemsOnlyEnv.items.length, 2);
  assert.equal(itemsOnlyEnv.items.some((line) => String(line).includes("[object Object]")), false);

  const structuredRows = logsMod.normalizeDockerLogEntries(logPayload, "aoc-world-riverlands").items;
  assert.equal(structuredRows.length, 2);
  assert.equal(structuredRows[0].component, "world");
  assert.equal(structuredRows[0].message, "NPC looks ready");
  assert.equal(structuredRows.some((r) => r.message.includes("[object Object]") || r.raw.includes("[object Object]")), false);

  const snap = logsMod.normalizeContainersScrapeResponse({
    world_status: { players: 5, npcs: 200, monsters: 45, resource_nodes: 80, mounts: 12, pois: 27 },
    workers: [{ worker_id: "world-node-winstead", scope_kind: "node", scope_key: "node:Winstead", npcs: 180, monsters: 20, resource_nodes: 45, mounts: 3, active_players: 2 }],
  });
  assert.equal(snap.world_status.total_entities, 342);
  assert.equal(snap.world_status.players_online, 5);
  assert.equal(snap.world_status.npcs_total, 200);
  assert.equal(snap.world_status.monsters_total, 45);
  assert.equal(snap.world_status.resource_nodes_total, 80);
  assert.equal(snap.world_status.mounts_total, 12);
  assert.equal(snap.workers.length, 1);
  assert.equal(snap.workers[0].total_entities, 250);
  assert.equal(snap.workers[0].tick_hz, 60);
  assert.equal(snap.workers[0].scope_tier_badge, "NODE_PLUG");

  const logsSrc = readFileSync(fileURLToPath(new URL("./LogsAndContainersTab.tsx", import.meta.url)), "utf8");
  assert.equal(logsSrc.includes('metric-total-entities'), true);
  assert.equal(logsSrc.includes("NPCs / Monsters"), false);
}

async function testTopLeftCityQuickJumpBottomRightWorkersAndActionToasts(): Promise<void> {
  const storeMod = await import("../state/worldStreamStore.ts");
  const mapSrc = readFileSync(fileURLToPath(new URL("../map/VerraMapCanvas.tsx", import.meta.url)), "utf8");
  const logsSrc = readFileSync(fileURLToPath(new URL("./LogsAndContainersTab.tsx", import.meta.url)), "utf8");
  const statusSrc = readFileSync(fileURLToPath(new URL("./StreamStatus.tsx", import.meta.url)), "utf8");
  const spawnerSrc = readFileSync(fileURLToPath(new URL("./PlayersAndSpawnerTab.tsx", import.meta.url)), "utf8");

  assert.equal(mapSrc.includes('left: "12px", top: "12px"'), true);
  assert.equal(mapSrc.includes('data-testid="city-quick-jump-combobox"'), true);
  assert.equal(mapSrc.includes('data-testid="city-quick-jump-search"'), true);
  assert.equal(mapSrc.includes('data-testid="city-node-quick-select"'), true);
  assert.equal(mapSrc.includes('bottom: "12px", right: "12px"'), true);
  assert.equal(mapSrc.includes('data-testid="gm-action-toast-stack"'), true);
  assert.equal(mapSrc.includes('data-testid="visible-server-workers-panel"'), true);
  assert.equal(mapSrc.includes("worker-tick-rate-select-"), true);
  assert.equal(logsSrc.includes("analytics-worker-tick-select-"), true);
  assert.equal(mapSrc.includes('height: "208px", minHeight: "208px", maxHeight: "208px"'), true);
  assert.equal(mapSrc.includes('height: "176px", minHeight: "176px", maxHeight: "176px"'), true);
  assert.equal(mapSrc.includes('height: "30px", minHeight: "30px", maxHeight: "30px"'), true);
  assert.equal(mapSrc.includes('scrollbarGutter: "stable"'), true);
  assert.equal(mapSrc.includes('position: "relative", zIndex: 60'), true);
  assert.equal(mapSrc.includes('zIndex: 120'), true);
  const visibleDefault = storeMod.getVisibleWorkersForViewport(storeMod.DEFAULT_REGIONAL_WORLD_WORKERS, storeMod.computeViewportWorldBoundsCm(storeMod.DEFAULT_CAMERA_VIEWPORT));
  assert.ok(visibleDefault.length >= 1 && visibleDefault.length <= storeMod.DEFAULT_REGIONAL_WORLD_WORKERS.length);
  assert.equal(mapSrc.includes('data-testid="inspector-give-item-btn"'), true);
  assert.equal(mapSrc.includes('data-testid="inspector-give-gold-btn"'), true);
  assert.equal(mapSrc.includes('data-testid="npc-instance-nudge"'), true);
  assert.equal(statusSrc.includes('data-testid="camera-mode-toggle"'), false);
  assert.equal(spawnerSrc.includes("open-modal-"), false);
  assert.ok(mapSrc.indexOf('data-testid="gm-action-toast-stack"') < mapSrc.indexOf('data-testid="visible-server-workers-panel"'));

  const duunholdMatches = storeMod.DEFAULT_VERRA_CITY_NODES.filter((n) => `${n.display_name} ${n.name}`.toLowerCase().includes("duunhold"));
  const sunhavenMatches = storeMod.DEFAULT_VERRA_CITY_NODES.filter((n) => `${n.display_name} ${n.name}`.toLowerCase().includes("sunhaven"));
  assert.equal(duunholdMatches[0]?.display_name, "Duunhold");
  assert.equal(sunhavenMatches[0]?.display_name, "Sunhaven");

  const store = storeMod.createWorldStreamStore({ workers: storeMod.DEFAULT_REGIONAL_WORLD_WORKERS });
  store.getState().setScopedReceipt("nodeLevel", { ok: true, message: "Duunhold -> Lv 6: executed" });
  store.getState().setScopedReceipt("nodeXp", { ok: true, message: "Duunhold +10000 XP: add_node_xp executed" });
  assert.equal(store.getState().toasts.some((t) => t.text === "Duunhold -> Lv 6: executed"), true);
  assert.equal(store.getState().toasts.some((t) => t.text === "Duunhold +10000 XP: add_node_xp executed"), true);

  let tickCmdUrl = "", tickCmdBody = "";
  await store.getState().executeSetWorkerTickRateAction("world-riverlands", 999, async (input, init) => {
    tickCmdUrl = String(input);
    tickCmdBody = String(init?.body ?? "");
    return new Response(JSON.stringify({ ok: true }), { status: 200 });
  });
  assert.equal(tickCmdUrl, "/api/v1/gm/command");
  assert.equal(JSON.parse(tickCmdBody).command, "set_tick_rate");
  assert.equal(JSON.parse(tickCmdBody).tick_hz, 60);
  assert.equal(store.getState().workers.find((w) => w.worker_id === "world-riverlands")?.tick_hz, 60);
  assert.equal(store.getState().lastReceiptsByScope.worker_tick?.message, "Set world-riverlands tick rate to 60 Hz");
  assert.equal(store.getState().toasts.some((t) => t.text === "Set world-riverlands tick rate to 60 Hz"), true);

  store.getState().setCameraViewport({ centerXCm: -644519, centerYCm: 376773, zoom: 6.0 });
  const visibleWorkers = storeMod.getVisibleWorkersForViewport(store.getState().workers, storeMod.computeViewportWorldBoundsCm(store.getState().camera));
  assert.ok(visibleWorkers.length >= 1 && visibleWorkers[0].worker_id === "world-riverlands");
  const zoomedArea = storeMod.zoomToWorkerArea(store.getState().workers[1], store.getState());
  assert.equal(store.getState().camera.centerXCm, zoomedArea.centerXCm);
  assert.equal(store.getState().camera.zoom, zoomedArea.targetZoom);
  const sandsquallVisible = storeMod.getVisibleWorkersForViewport(store.getState().workers, storeMod.computeViewportWorldBoundsCm(store.getState().camera));
  assert.equal(sandsquallVisible.some((w) => w.worker_id === "world-sandsquall"), true);
  assert.equal(sandsquallVisible.some((w) => w.worker_id === "world-turquoise-sea"), false);
}

async function testBackoffCircuitBreakerLoginResetAndScrollbarGutter(): Promise<void> {
  const authMod = await import("../state/adminAuthClient.ts");
  const storeMod = await import("../state/worldStreamStore.ts");
  const mapMod = await import("../map/VerraMapCanvas.tsx");
  const indexHtml = readFileSync(fileURLToPath(new URL("../../index.html", import.meta.url)), "utf8");
  const mainSrc = readFileSync(fileURLToPath(new URL("../main.tsx", import.meta.url)), "utf8");
  const govSrc = readFileSync(fileURLToPath(new URL("./NodeGovernanceAndSpawnModals.tsx", import.meta.url)), "utf8");

  assert.equal(indexHtml.includes("scrollbar-gutter: stable"), true);
  assert.equal(mainSrc.includes('scrollbarGutter: "stable"'), true);
  assert.equal(govSrc.includes('data-testid="city-node-teleport-here-btn"'), true);
  assert.equal(govSrc.includes('data-testid="city-node-spawn-here-btn"'), true);
  storeMod.useWorldStreamStore.getState().openCommandModal("teleport", { xCm: 10, yCm: 20, zCm: 30 });
  assert.equal(storeMod.useWorldStreamStore.getState().activeModal, "teleport");
  storeMod.useWorldStreamStore.getState().openCommandModal("spawn", { xCm: 40, yCm: 50, zCm: 60 });
  assert.equal(storeMod.useWorldStreamStore.getState().activeModal, "spawn-npc");
  assert.equal(storeMod.DEFAULT_CAMERA_VIEWPORT.zoom, 3.5);
  assert.equal(storeMod.DEFAULT_CAMERA_VIEWPORT.centerXCm, -777664.0);
  assert.equal(storeMod.DEFAULT_CAMERA_VIEWPORT.centerYCm, 522023.0);
  assert.equal(authMod.RETRY_BASE_DELAY_MS, 500);
  assert.equal(authMod.RETRY_MAX_DELAY_MS, 30000);
  assert.equal(authMod.RETRY_MAX_ATTEMPTS, 6);
  assert.equal(authMod.CIRCUIT_FAILURE_THRESHOLD, 5);
  assert.equal(authMod.CIRCUIT_OPEN_COOLDOWN_MS, 15000);
  assert.equal(authMod.computeExponentialBackoffMs(0, 500, 30000, () => 1.0), 500);
  assert.equal(authMod.computeExponentialBackoffMs(10, 500, 30000, () => 1.0), 30000);

  for (let i = 0; i < 5; i += 1) authMod.recordRetryFailure(1000);
  assert.equal(authMod.isRetryCircuitOpen(1000), true);
  const epBefore = authMod.getRetryResetEpoch();
  storeMod.useWorldStreamStore.getState().setCameraViewport({ centerXCm: -200000, centerYCm: 900000, zoom: 6.0 });
  authMod.clearAdminSession();
  assert.ok(authMod.getRetryResetEpoch() > epBefore);
  assert.equal(authMod.isRetryCircuitOpen(1000), false);
  assert.equal(authMod.isUnauthenticatedViewOnly(), true);
  assert.equal(storeMod.useWorldStreamStore.getState().camera.zoom, 3.5);
  assert.equal(storeMod.useWorldStreamStore.getState().cityNodes.length, 0);
  assert.equal(storeMod.useWorldStreamStore.getState().workers.length, 0);

  let clipCallsWhileLoggedOut = 0;
  const mockCtx = {
    set fillStyle(_v: string) {}, fillRect: () => {}, strokeRect: () => {},
    clip: () => { clipCallsWhileLoggedOut += 1; }, drawImage: () => {}, beginPath: () => {}, closePath: () => {},
    setLineDash: () => {}, arc: () => {}, fill: () => {}, stroke: () => {}, moveTo: () => {}, lineTo: () => {},
    rect: () => {}, fillText: () => {}, measureText: () => ({ width: 20 }), save: () => {}, restore: () => {}, translate: () => {}, rotate: () => {},
  } as unknown as CanvasRenderingContext2D;
  mapMod.drawCanvasScene(mockCtx, { width: 960, height: 540 }, storeMod.DEFAULT_VERRA_MAP_META, storeMod.useWorldStreamStore.getState(), new mapMod.VerraTileCache(() => {}));
  assert.equal(clipCallsWhileLoggedOut, 0);

  await authMod.loginWithPassphrase("admin", "pass", async () => new Response(JSON.stringify({ token: "jwt-ok", username: "admin", role: "GM_ARCHITECT", boot_id: "b1" }), { status: 200 }));
  assert.equal(authMod.isUnauthenticatedViewOnly(), false);
  assert.equal(storeMod.useWorldStreamStore.getState().cityNodes.length, 26);
  assert.equal(storeMod.useWorldStreamStore.getState().workers.length, 0);
  const epLogin = authMod.getRetryResetEpoch();
  storeMod.useWorldStreamStore.getState().setConnectionState("live");
  assert.ok(authMod.getRetryResetEpoch() > epLogin);
}

async function testSessionStorageEtagRevalidationAutoFollowAndTimeOfDay(): Promise<void> {
  const storeMod = await import("../state/worldStreamStore.ts");
  const paletteMod = await import("./GmCommandPalette.tsx");
  const logsMod = await import("./LogsAndContainersTab.tsx");
  const mapMod = await import("../map/VerraMapCanvas.tsx");
  const inspectorMod = await import("./EntityInspector.tsx");
  const mapSrc = readFileSync(fileURLToPath(new URL("../map/VerraMapCanvas.tsx", import.meta.url)), "utf8");

  assert.equal(logsMod.areContainerNamesEquivalent("aoc-riverlands", "aoc-world-riverlands"), true);
  assert.equal(logsMod.formatClusterDisplayName("mesh_gateway_http"), "Mesh Gateway (HTTP)");
  assert.equal(logsMod.formatClusterDisplayName("gateway_grpc"), "Gateway Service (gRPC)");
  assert.equal(logsMod.formatClusterDisplayName("gateway_tls"), "Gateway Service (TLS)");
  assert.equal(logsMod.formatClusterDisplayName("world_admin_grpc"), "World Admin Service (gRPC)");
  assert.equal(logsMod.formatClusterDisplayName("ingress_downstream"), "Ingress Proxy (HTTPS/TLS)");
  assert.equal(logsMod.formatClusterDisplayName("web_admin_http"), "Web Admin UI (HTTP)");
  const parsedResNodes = mapMod.parseWorldStateResponse(
    { entities: [{ guid: "res-101", name: "Iron Ore Vein", kind: "resource_node", x_cm: -644000, y_cm: 376000, z_cm: 12000, resource_type: "Mining / Ore Vein", quality: "Rare · Tier 2 (Apprentice)" }] },
    storeMod.DEFAULT_LAYER_TOGGLES,
  );
  assert.equal(parsedResNodes.length, 1);
  assert.equal(parsedResNodes[0].resourceType, "Mining / Ore Vein");
  assert.equal(parsedResNodes[0].quality, "Rare · Tier 2 (Apprentice)");
  const resInspector = inspectorMod.buildEntityInspectorModel({ entity: parsedResNodes[0] });
  assert.equal(resInspector.rows.find((r) => r.label === "Resource Type")?.value, "Mining / Ore Vein");
  assert.equal(resInspector.rows.find((r) => r.label === "Quality / Rarity")?.value, "Rare · Tier 2 (Apprentice)");
  assert.equal(mapSrc.includes('data-testid="selected-resource-type"'), true);
  assert.equal(mapSrc.includes('data-testid="selected-resource-quality"'), true);
  assert.equal(logsMod.computeNextAutoFollowOnScroll(100, 500, 200, true), false);
  assert.equal(logsMod.computeNextAutoFollowOnScroll(300, 500, 200, false), true);
  assert.ok(logsMod.AUTO_TAIL_TOOLTIP.includes("Auto Tail") && logsMod.AUTO_FOLLOW_TOOLTIP.includes("Auto Follow"));
  assert.equal(mapSrc.includes('data-testid="npc-nudge-amount-input"'), true);
  assert.equal(mapSrc.includes('data-testid="npc-instance-nudge-y"'), true);
  assert.equal(mapSrc.includes('data-testid="npc-instance-nudge-z"'), true);
  assert.equal(mapSrc.includes('data-testid="world-current-time-display"'), true);
  assert.equal(mapSrc.includes('data-testid="world-time-picker-btn"'), true);
  assert.equal(mapSrc.includes('data-testid="world-time-picker-popover"'), true);
  assert.equal(mapSrc.includes('data-testid="world-time-slider"'), true);
  assert.equal(mapSrc.includes('data-testid="world-time-apply-btn"'), true);
  assert.equal(mapSrc.includes('data-testid="world-time-of-day-select"'), true);
  assert.equal(mapSrc.includes("Time {liveClock}"), true);
  assert.equal(mapSrc.includes("Set TOD"), false);
  assert.equal(mapSrc.includes("Zoom {storeSnap.camera.zoom.toFixed(1)}"), true);
  assert.equal(storeMod.formatTimeOfDayClock(storeMod.computeLiveTimeOfDayHours(12.0, 1000, 11000)), "12:01:30");
  assert.equal(storeMod.formatTimePhaseLabel(18), "Dusk");
  assert.equal(storeMod.parseTimeOfDayInput("18:30"), 18.5);
  assert.equal(storeMod.parseTimeOfDayInput("6"), 6.0);

  paletteMod.clearEtagResponseCache();
  let seenIfNoneMatch = "";
  const firstRes = await paletteMod.fetchWithTransientRetry("/api/v1/gm/logs?tail=100", {}, {
    fetchImpl: async () => new Response('{"lines":["a"]}', { status: 200, headers: { ETag: '"etag-v1"' } }),
  });
  assert.equal(firstRes.status, 200);
  const secondRes = await paletteMod.fetchWithTransientRetry("/api/v1/gm/logs?tail=100", {}, {
    fetchImpl: async (_url, init) => {
      seenIfNoneMatch = String((init?.headers as Record<string, string>)?.["If-None-Match"] ?? "");
      return new Response(null, { status: 304 });
    },
  });
  assert.equal(seenIfNoneMatch, '"etag-v1"');
  assert.equal(secondRes.status, 200);
  assert.equal(secondRes.headers.get("X-Cache"), "HIT-304");
  assert.equal(await secondRes.text(), '{"lines":["a"]}');

  const s = storeMod.useWorldStreamStore.getState();
  s.setCameraViewport({ centerXCm: -500000, centerYCm: 400000, zoom: 5.5 });
  s.selectEntity("ent-session-99");
  s.patchSessionUi({ nudgeMeters: "-12.5", logAutoTail: false, logAutoFollow: false, collapsedSections: { shards: true } });
  await s.executeSetTimeOfDayAction(6.0, async () => new Response('{"ok":true}', { status: 200 }));
  assert.equal(storeMod.useWorldStreamStore.getState().timeOfDayHours, 6.0);
  assert.equal(storeMod.useWorldStreamStore.getState().timeOfDayLabel, "06:00");
  storeMod.useWorldStreamStore.getState().resetForUnauthenticatedSession();
  assert.equal(storeMod.useWorldStreamStore.getState().camera.zoom, 3.5);
  assert.equal(storeMod.useWorldStreamStore.getState().selectedEntityGuid, null);
  storeMod.useWorldStreamStore.getState().restoreForAuthenticatedSession();
  assert.equal(storeMod.useWorldStreamStore.getState().camera.zoom, 5.5);
  assert.equal(storeMod.useWorldStreamStore.getState().selectedEntityGuid, "ent-session-99");
  assert.equal(storeMod.useWorldStreamStore.getState().nudgeMeters, "-12.5");
  assert.equal(storeMod.useWorldStreamStore.getState().logAutoTail, false);
  assert.equal(storeMod.useWorldStreamStore.getState().logAutoFollow, false);
  assert.equal(storeMod.useWorldStreamStore.getState().collapsedSections.shards, true);
  assert.equal(storeMod.useWorldStreamStore.getState().timeOfDayHours, 6.0);
}

async function runAllGmActionsContracts(): Promise<void> {
  await testVerraCoordinateMathAndDynamicTilePyramid();
  await testNodePlugTransformAndAuthenticCityNodeStageIcons();
  await testZoiPolygonsVhalgadimClippingAndBlackCanvas();
  await testMutuallyExclusiveSelectionAndRightClickClear();
  await testScopedReceiptsMayorElectionsAndPersistence();
  await testStreamLayerBitmasksPoisAndFindInWorldSearch();
  await testWorkerAuthorityResolutionAndCanonicalNames();
  await testGmCommandBuildersPaginationAndTransientRetry();
  await testInspectorModalActionsCatalogNpcsAndNpcRemoval();
  await testScopedJwtAdminLoginAnd401Or403ReLogin();
  await testZeroObjectLogFalsificationAndConsistentEntityTaxonomy();
  await testTopLeftCityQuickJumpBottomRightWorkersAndActionToasts();
  await testBackoffCircuitBreakerLoginResetAndScrollbarGutter();
  await testSessionStorageEtagRevalidationAutoFollowAndTimeOfDay();
  console.log("PASS: gm_actions unit and layer bitmask contract suite (14/14 groups)");
}

if (process.env.VITEST) {
  const { describe, it } = await import("vitest");
  describe("gm_actions unit and layer bitmask contract suite", () => {
    it("verifies all 14 contract groups", async () => {
      await runAllGmActionsContracts();
    });
  });
} else {
  await runAllGmActionsContracts();
}

