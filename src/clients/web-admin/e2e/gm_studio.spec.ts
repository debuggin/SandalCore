// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { registerHooks } from "node:module";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

registerHooks({
  resolve(specifier, context, nextResolve) {
    if (
      specifier === "react" ||
      specifier === "react-dom" ||
      specifier === "react-dom/client" ||
      specifier === "react/jsx-runtime" ||
      specifier === "react/jsx-dev-runtime"
    ) {
      return {
        url: "data:text/javascript,export const useState = (v) => [typeof v === 'function' ? v() : v, () => {}]; export const useEffect = () => {}; export const useMemo = (fn) => fn(); export const useCallback = (fn) => fn; export const useRef = (v) => ({ current: v }); export const createPortal = (node) => node; export const createElement = () => null; export const jsx = () => null; export const jsxs = () => null; export const jsxDEV = () => null; export const Fragment = 'Fragment'; export class Component { constructor(props) { this.props = props; } } export const createRoot = () => ({ render: () => {} }); export default { useState, useEffect, useMemo, useCallback, useRef, createPortal, createElement, jsx, jsxs, jsxDEV, Fragment, Component, StrictMode: 'div' };",
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

async function step1ThreeTabShellAndClutterRemoval(): Promise<void> {
  const mainMod = await import("../src/main.tsx");
  assert.equal(mainMod.ADMIN_TABS.length, 2);
  assert.deepEqual(
    mainMod.ADMIN_TABS.map((t: { id: string; label: string }) => ({
      id: t.id,
      label: t.label,
    })),
    [
      { id: "map", label: "Map" },
      { id: "analytics", label: "Analytics & Logs" },
    ],
  );

  const webAdminRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
  assert.equal(
    existsSync(resolve(webAdminRoot, "src/re/ReInspector.tsx")),
    false,
  );
  assert.equal(
    existsSync(resolve(webAdminRoot, "src/components/HotfixEditor.tsx")),
    false,
  );
  assert.equal(
    existsSync(resolve(webAdminRoot, "src/components/AuditRollback.tsx")),
    false,
  );
}

async function step2WorldMapTilesNodePlugsAndLayerMaskPolling(): Promise<void> {
  const mapMod = await import("../src/map/VerraMapCanvas.tsx");
  const storeMod = await import("../src/state/worldStreamStore.ts");

  assert.equal(mapMod.DEFAULT_VERRA_PMTILES_URL, "/api/v1/world/pmtiles");
  assert.equal(mapMod.PURE_BLACK_TILE_BG, "#000000");
  assert.equal(mapMod.VERRA_ZOI_POLYGONS.length, 55);
  assert.equal(mapMod.VERRA_ZOI_POLYGONS.filter((z) => z.has_in_game_border).length, 27);
  assert.equal(mapMod.VERRA_ZOI_POLYGONS.filter((z) => !z.has_in_game_border).length, 28);
  assert.equal(mapMod.VERRA_ZOI_POLYGONS.find((z) => z.zoi_index === 49)?.has_in_game_border, true);
  assert.equal(mapMod.VERRA_ZOI_POLYGONS.find((z) => z.zoi_index === 36)?.has_in_game_border, false);

  const vhalgadim = mapMod.VERRA_ZOI_POLYGONS.find((z) => z.zoi_index === 44);
  assert.ok(vhalgadim !== undefined && vhalgadim.has_in_game_border);
  for (const ring of vhalgadim.rings) {
    for (const [u, v] of ring) {
      if (v <= 8704) assert.ok(u <= 17408);
    }
  }
  assert.equal(mapMod.resolveCityNodeIconName({ level: 0 }), "TUI_Icon_Node_LVL0");
  assert.equal(mapMod.resolveCityNodeIconName({ level: 1 }), "TUI_Icon_Node_LVL0");
  assert.equal(mapMod.resolveCityNodeIconName({ level: 2 }), "TUI_Icon_Node_LVL1");
  assert.equal(mapMod.resolveCityNodeIconName({ level: 3 }), "TUI_Icon_Node_LVL2");
  assert.equal(mapMod.resolveCityNodeIconName({ level: 4 }), "TUI_Icon_Node_LVL3");
  assert.equal(mapMod.resolveCityNodeIconName({ level: 5 }), "TUI_Icon_Node_LVL4");
  assert.equal(mapMod.resolveCityNodeIconName({ level: 6 }), "TUI_Icon_Node_Sovereignty");

  const winsteadLv0 = { ...storeMod.DEFAULT_VERRA_CITY_NODES[0], level: 0 };
  const winsteadPlug = storeMod.computeNodePlugTileOverlay(winsteadLv0);
  assert.equal(winsteadPlug?.plugTier, 0);
  const winsteadCenter = storeMod.getCityNodeCenterCm(winsteadLv0);
  assert.equal(winsteadCenter.cx_cm, winsteadPlug?.cxCm);
  assert.equal(winsteadCenter.cy_cm, winsteadPlug?.cyCm);
  const [lng, lat] = storeMod.verraUnrealToLngLat(128000.5, -64000.25);
  const roundTrip = storeMod.lngLatToVerraUnreal(lng, lat, 512.0);
  assert.ok(Math.abs(roundTrip.xCm - 128000.5) < 1e-6 && Math.abs(roundTrip.yCm - -64000.25) < 1e-6);

  for (const [zoom, expectedZ] of [[0.0, 3], [2.0, 5], [3.5, 7], [5.0, 7], [7.0, 7], [10.0, 7]] as const) {
    const tiles = storeMod.computeVisibleBaseTiles(storeMod.DEFAULT_VERRA_MAP_META, -777664, 522023, zoom, { width: 1024, height: 768 });
    assert.ok(tiles.length > 0 && tiles.every((t) => t.z === expectedZ));
  }
  assert.equal(storeMod.getMapPollHzForZoom(0), 30);
  assert.equal(storeMod.getMapPollIntervalMsForZoom(0), 33);

  const fullLayers = { players: true, npcs: true, monsters: true, resource_nodes: true, mounts: true, loot_piles: true, pois: true };
  assert.equal(storeMod.computeStreamLayerMask(fullLayers), 127);
  assert.equal(
    storeMod.buildWorldStatePollUrl(fullLayers),
    "/api/v1/world/state?layer_mask=127&layers=players,npcs,monsters,resource_nodes,mounts,loot_piles,pois",
  );
  const withoutMonstersAndPois = { ...fullLayers, monsters: false, pois: false };
  assert.equal(storeMod.computeStreamLayerMask(withoutMonstersAndPois), storeMod.STREAM_LAYER_ALL);
  assert.equal(storeMod.shouldFetchPoisCatalog(withoutMonstersAndPois), false);
  const allOff = { players: false, npcs: false, monsters: false, resource_nodes: false, mounts: false, loot_piles: false, pois: false };
  assert.equal(storeMod.computeStreamLayerMask(allOff), 0);
  assert.equal(storeMod.shouldPollWorldState(allOff), false);
  assert.equal(storeMod.buildWorldStatePollUrl(allOff), null);
}

async function step3PlayersAndSpawnerGmActions(): Promise<void> {
  const paletteMod = await import("../src/components/GmCommandPalette.tsx");
  const tab2Mod = await import("../src/components/PlayersAndSpawnerTab.tsx");
  const inspectorMod = await import("../src/components/EntityInspector.tsx");
  const storeMod = await import("../src/state/worldStreamStore.ts");

  assert.equal(typeof tab2Mod.PlayersAndSpawnerTab, "function");
  assert.equal(typeof tab2Mod.CatalogComboboxPicker, "function");
  assert.equal(tab2Mod.DEFAULT_ONLINE_PLAYERS.length, 0);
  assert.deepEqual(tab2Mod.DEFAULT_CATALOG_NPCS.map((n) => n.name), ["Aela", "Riverlands Wolf", "Citizen"]);
  assert.deepEqual(paletteMod.ITEM_RARITIES, ["Common", "Uncommon", "Rare", "Heroic", "Epic", "Legendary"]);

  const tpCmd = paletteMod.buildTeleportPlayerCommand({ character: "Cyb", target_player: "Hatemost" });
  assert.equal(tpCmd.command, "teleport_player");
  assert.equal("target_player" in tpCmd ? tpCmd.target_player : "", "Hatemost");

  const grantCmd = paletteMod.buildGrantItemCommand("Cyb", "60000000123", 3, "Epic");
  assert.equal(grantCmd.command, "grant_item");
  assert.equal(grantCmd.count, 3);
  assert.equal(grantCmd.rarity, "Epic");

  const store = storeMod.createWorldStreamStore();
  await store.getState().setNodeMayor("Verra_RVR_Winstead", "Hatemost", async () =>
    new Response(JSON.stringify({ message: "Mayor assigned" }), { status: 200 }),
  );
  assert.equal(store.getState().cityNodes[0].mayor_name, "Hatemost");
  assert.equal(store.getState().lastReceiptsByScope.nodeMayor?.message, "Mayor assigned");
  assert.equal(store.getState().lastReceiptsByScope.teleport, null);

  store.getState().applyWorldDelta({
    seq: 1,
    isSnapshot: true,
    added: [
      { guid: "e2e-npc-1", name: "Aela", kind: "npc", xCm: -644519, yCm: 376773, zCm: 12012, yawDeg: 0, level: 15, healthPct: 100 },
      { guid: "e2e-npc-2", name: "Riverlands Wolf", kind: "monster", xCm: -644000, yCm: 376000, zCm: 12000, yawDeg: 0, level: 12, healthPct: 100 },
    ],
  });
  store.getState().selectEntity("e2e-npc-1");
  const inspected = store.getState().entities.get("e2e-npc-1")!;
  inspectorMod.openTeleportModalFromEntity(inspected, store.getState());
  assert.equal(store.getState().activeModal, "teleport");
  assert.equal(store.getState().teleportMode, "coords");
  assert.deepEqual(store.getState().pickedCoordinate, { xCm: -644519, yCm: 376773, zCm: 12012 });

  inspectorMod.openSpawnNpcModalFromEntity(inspected, store.getState());
  assert.equal(store.getState().activeModal, "spawn-npc");
  assert.equal(store.getState().spawnTargetMode, "coords");
  assert.deepEqual(store.getState().pickedCoordinate, { xCm: -644519, yCm: 376773, zCm: 12012 });

  let statePolls = 0;
  const mockFetch: typeof fetch = async (input) => {
    if (String(input).includes("/api/v1/world/state")) {
      statePolls += 1;
      return new Response(JSON.stringify({ entities: [] }), { status: 200 });
    }
    return new Response(JSON.stringify({ ok: true, message: "ok" }), { status: 200 });
  };
  await tab2Mod.dispatchAuxiliaryGmAction({ action: "kill_npc", entity_guid: "e2e-npc-1" }, mockFetch, store.getState());
  assert.equal(store.getState().entities.has("e2e-npc-1"), false);
  await tab2Mod.dispatchAuxiliaryGmAction({ action: "despawn_npc", entity_guid: "e2e-npc-2" }, mockFetch, store.getState());
  assert.equal(store.getState().entities.has("e2e-npc-2"), false);
  assert.equal(statePolls, 2);
}

async function step4LogsAndElevenContainerAnalytics(): Promise<void> {
  const paletteMod = await import("../src/components/GmCommandPalette.tsx");
  const tab3Mod = await import("../src/components/LogsAndContainersTab.tsx");

  assert.equal(typeof tab3Mod.LogsAndContainersTab, "function");
  assert.equal(tab3Mod.CANONICAL_SANDAL_CONTAINERS.length, 11);
  assert.equal(tab3Mod.CANONICAL_SANDAL_CONTAINERS.includes("aoc-tether" as never), false);
  assert.equal(tab3Mod.CANONICAL_SANDAL_CONTAINERS.includes("aoc-world-riverlands"), true);
  assert.equal(tab3Mod.CANONICAL_SANDAL_CONTAINERS.includes("aoc-mesh-gateway"), true);

  const normalized = tab3Mod.normalizeContainersScrapeResponse({
    world_status: { players: 4, npcs: 12450, monsters: 320, resource_nodes: 810, mounts: 16 },
    gateway_telemetry: {
      requests_total: 250, errors_total: 1, cache_hits_total: 180, cache_misses_total: 70, transient_retries_total: 2,
    },
    containers: [{
      name: "aoc-world-riverlands", state: "running", status: "Up 15 minutes",
      cpu_pct: 14.5, mem_mb: 1840.2, mem_limit_mb: 8192, rx_bytes: 1048576, tx_bytes: 2097152,
    }],
  });
  assert.equal(normalized.world_status.total_entities, 13600);
  assert.equal(normalized.world_status.players_online, 4);
  assert.equal(normalized.world_status.npcs_total, 12450);
  assert.equal(normalized.world_status.monsters_total, 320);
  assert.equal(normalized.world_status.resource_nodes_total, 810);
  assert.equal(normalized.world_status.mounts_total, 16);
  assert.equal(normalized.gateway_telemetry.requests_total, 250);
  assert.equal(normalized.gateway_telemetry.transient_retries_total, 2);
  assert.equal(normalized.prometheus.targets.length, 3);
  assert.equal(normalized.containers.length, 1);
  assert.equal(tab3Mod.normalizeContainersScrapeResponse({}).containers.length, 11);

  const logEnvelope = {
    items: [{ ts: "2026-11-07T09:00:00Z", timestamp: "2026-11-07T09:00:00Z", stream: "stdout", container: "aoc-world-riverlands", message: '{"level":"INFO","component":"world","msg":"NPC looks ready"}' }],
    lines: ['2026-11-07T09:00:00Z [stdout] {"level":"INFO","component":"world","msg":"NPC looks ready"}'],
  };
  const paletteLines = paletteMod.normalizePaginatedEnvelope<string>(logEnvelope, ["lines", "items"]);
  assert.equal(paletteLines.items.some((s) => String(s).includes("[object Object]")), false);
  const dockerRows = tab3Mod.normalizeDockerLogEntries(logEnvelope, "aoc-world-riverlands").items;
  assert.equal(dockerRows.length, 1);
  assert.equal(dockerRows[0].message, "NPC looks ready");
  assert.equal(dockerRows[0].raw.includes("[object Object]"), false);

  const logsUrl = paletteMod.buildPaginatedQueryUrl("/api/v1/gm/logs", {
    q: "NPC looks", limit: 100, offset: 0, extra: { container: "aoc-world-riverlands", tail: 200 },
  });
  assert.equal(logsUrl, "/api/v1/gm/logs?q=NPC%20looks&limit=100&offset=0&container=aoc-world-riverlands&tail=200");
}

async function step5JwtLoginQuickJumpWorkersAndActionToasts(): Promise<void> {
  const authMod = await import("../src/state/adminAuthClient.ts");
  const loginModalMod = await import("../src/components/AdminLoginModal.tsx");
  const storeMod = await import("../src/state/worldStreamStore.ts");

  assert.equal(typeof loginModalMod.AdminLoginModal, "function");
  authMod.clearAdminSession();
  const loginRes = await authMod.loginWithPassphrase("admin", "passphrase", async () =>
    new Response(
      JSON.stringify({ token: "e2e-jwt-token", username: "admin", role: "GM_ADMIN", scopes: ["admin:read", "admin:write"], boot_id: "boot-1" }),
      { status: 200 },
    ),
  );
  assert.equal(loginRes.ok, true);
  assert.equal(authMod.buildAdminAuthHeaders().Authorization, "Bearer e2e-jwt-token");
  authMod.handleUnauthorizedStatus(401, "Server restarted (boot_id changed)");
  assert.equal(authMod.getAdminAuthState().loginPromptOpen, true);

  const webAdminRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
  const mapSrc = readFileSync(resolve(webAdminRoot, "src/map/VerraMapCanvas.tsx"), "utf8");
  const logsSrc = readFileSync(resolve(webAdminRoot, "src/components/LogsAndContainersTab.tsx"), "utf8");
  assert.equal(mapSrc.includes('left: "12px", top: "12px"'), true);
  assert.equal(mapSrc.includes('data-testid="city-quick-jump-combobox"'), true);
  assert.equal(mapSrc.includes('data-testid="city-quick-jump-search"'), true);
  assert.equal(mapSrc.includes('data-testid="city-node-quick-select"'), true);
  assert.equal(mapSrc.includes('data-testid="gm-action-toast-stack"'), true);
  assert.equal(mapSrc.includes('data-testid="visible-server-workers-panel"'), true);
  assert.equal(mapSrc.includes("worker-tick-rate-select-"), true);
  assert.equal(mapSrc.includes('scrollbarGutter: "stable"'), true);
  assert.equal(mapSrc.includes('position: "relative", zIndex: 60'), true);
  assert.equal(mapSrc.includes('zIndex: 120'), true);
  assert.ok(storeMod.getVisibleWorkersForViewport(storeMod.DEFAULT_REGIONAL_WORLD_WORKERS, storeMod.computeViewportWorldBoundsCm(storeMod.DEFAULT_CAMERA_VIEWPORT)).length > 6);
  assert.equal(logsSrc.includes("analytics-worker-tick-select-"), true);
  assert.equal(logsSrc.includes('metric-total-entities'), true);
  assert.equal(logsSrc.includes("NPCs / Monsters"), false);

  const store = storeMod.createWorldStreamStore();
  store.getState().setScopedReceipt("nodeLevel", { ok: true, message: "Duunhold -> Lv 6: executed" });
  store.getState().setScopedReceipt("nodeXp", { ok: true, message: "Duunhold +10000 XP: add_node_xp executed" });
  assert.equal(store.getState().toasts.some((t) => t.text === "Duunhold -> Lv 6: executed"), true);
  assert.equal(store.getState().toasts.some((t) => t.text === "Duunhold +10000 XP: add_node_xp executed"), true);

  let tickPostedCmd = "";
  await store.getState().executeSetWorkerTickRateAction("world-riverlands", 60, async (_url, init) => {
    tickPostedCmd = String(init?.body ?? "");
    return new Response(JSON.stringify({ ok: true }), { status: 200 });
  });
  assert.equal(JSON.parse(tickPostedCmd).command, "set_tick_rate");
  assert.equal(store.getState().workers.find((w) => w.worker_id === "world-riverlands")?.tick_hz, 60);
  assert.equal(store.getState().lastReceiptsByScope.worker_tick?.message, "Set world-riverlands tick rate to 60 Hz");

  let todPostedCmd = "";
  await store.getState().executeSetTimeOfDayAction(18.0, async (_url, init) => {
    todPostedCmd = String(init?.body ?? "");
    return new Response(JSON.stringify({ ok: true }), { status: 200 });
  });
  assert.equal(JSON.parse(todPostedCmd).command, "set_time_of_day");
  assert.equal(store.getState().timeOfDayHours, 18.0);
  assert.equal(store.getState().timeOfDayLabel, "18:00");

  const area = storeMod.zoomToWorkerArea(store.getState().workers[0], store.getState());
  assert.equal(store.getState().camera.centerXCm, area.centerXCm);
  assert.equal(store.getState().camera.zoom, area.targetZoom);
}

async function main(): Promise<void> {
  await step1ThreeTabShellAndClutterRemoval();
  await step2WorldMapTilesNodePlugsAndLayerMaskPolling();
  await step3PlayersAndSpawnerGmActions();
  await step4LogsAndElevenContainerAnalytics();
  await step5JwtLoginQuickJumpWorkersAndActionToasts();
  console.log("[PASS] gm_studio.spec.ts: all 5 E2E GM Studio workflow steps verified");
}

await main();
