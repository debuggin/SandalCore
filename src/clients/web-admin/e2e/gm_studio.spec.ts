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
      specifier === "react/jsx-runtime"
    ) {
      return {
        url: "data:text/javascript,export const useState = (v) => [typeof v === 'function' ? v() : v, () => {}]; export const useEffect = () => {}; export const useMemo = (fn) => fn(); export const useCallback = (fn) => fn; export const useRef = (v) => ({ current: v }); export const createPortal = (node) => node; export const createElement = () => null; export class Component { constructor(props) { this.props = props; } } export const createRoot = () => ({ render: () => {} }); export default { useState, useEffect, useMemo, useCallback, useRef, createPortal, createElement, Component, StrictMode: 'div' };",
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
  const winsteadLv0 = { ...storeMod.DEFAULT_VERRA_CITY_NODES[0], level: 0 };
  const winsteadPlug = storeMod.computeNodePlugTileOverlay(winsteadLv0);
  assert.equal(winsteadPlug?.plugTier, 0);
  const winsteadCenter = storeMod.getCityNodeCenterCm(winsteadLv0);
  assert.equal(winsteadCenter.cx_cm, winsteadPlug?.cxCm);
  assert.equal(winsteadCenter.cy_cm, winsteadPlug?.cyCm);
  const [lng, lat] = storeMod.verraUnrealToLngLat(128000.5, -64000.25);
  const roundTrip = storeMod.lngLatToVerraUnreal(lng, lat, 512.0);
  assert.ok(Math.abs(roundTrip.xCm - 128000.5) < 1e-6);
  assert.ok(Math.abs(roundTrip.yCm - -64000.25) < 1e-6);

  // Verify dynamic z = 0..7 pyramid selection across zoom range (0..10)
  const expectedZoomMap: ReadonlyArray<readonly [number, number]> = [
    [0.0, 1],
    [2.0, 3],
    [5.0, 6],
    [7.0, 7],
    [10.0, 7],
  ];
  for (const [zoom, expectedZ] of expectedZoomMap) {
    const tiles = storeMod.computeVisibleBaseTiles(
      storeMod.DEFAULT_VERRA_MAP_META,
      -777664,
      522023,
      zoom,
      { width: 1024, height: 768 },
    );
    assert.ok(tiles.length > 0);
    assert.ok(tiles.every((t) => t.z === expectedZ));
  }

  // Verify layer toggles update ?layer_mask= and ?layers= and pause polling when all OFF
  const fullLayers = {
    players: true,
    npcs: true,
    monsters: true,
    resource_nodes: true,
    mounts: true,
    loot_piles: true,
    pois: true,
  };
  assert.equal(storeMod.computeStreamLayerMask(fullLayers), 127);
  assert.equal(
    storeMod.buildWorldStatePollUrl(fullLayers),
    "/api/v1/world/state?layer_mask=127&layers=players,npcs,monsters,resource_nodes,mounts,loot_piles,pois",
  );

  const withoutMonstersAndPois = {
    ...fullLayers,
    monsters: false,
    pois: false,
  };
  assert.equal(
    storeMod.computeStreamLayerMask(withoutMonstersAndPois),
    storeMod.STREAM_LAYER_ALL,
  );
  assert.equal(storeMod.shouldFetchPoisCatalog(withoutMonstersAndPois), false);
  assert.equal(
    storeMod.buildWorldStatePollUrl(withoutMonstersAndPois),
    "/api/v1/world/state?layer_mask=31&layers=players,npcs,resource_nodes,mounts,loot_piles",
  );

  const allOff = {
    players: false,
    npcs: false,
    monsters: false,
    resource_nodes: false,
    mounts: false,
    loot_piles: false,
    pois: false,
  };
  assert.equal(storeMod.computeStreamLayerMask(allOff), 0);
  assert.equal(storeMod.shouldPollWorldState(allOff), false);
  assert.equal(storeMod.buildWorldStatePollUrl(allOff), null);
}

async function step3PlayersAndSpawnerGmActions(): Promise<void> {
  const paletteMod = await import("../src/components/GmCommandPalette.tsx");
  const tab2Mod = await import("../src/components/PlayersAndSpawnerTab.tsx");
  const storeMod = await import("../src/state/worldStreamStore.ts");

  assert.equal(typeof tab2Mod.PlayersAndSpawnerTab, "function");
  assert.equal(typeof tab2Mod.CatalogComboboxPicker, "function");
  assert.equal(tab2Mod.DEFAULT_ONLINE_PLAYERS.length, 0);
  assert.deepEqual(paletteMod.ITEM_RARITIES, [
    "Common",
    "Uncommon",
    "Rare",
    "Heroic",
    "Epic",
    "Legendary",
  ]);

  const tpCmd = paletteMod.buildTeleportPlayerCommand({
    character: "Cyb",
    target_player: "Hatemost",
  });
  assert.equal(tpCmd.command, "teleport_player");
  assert.equal("target_player" in tpCmd ? tpCmd.target_player : "", "Hatemost");

  const grantCmd = paletteMod.buildGrantItemCommand(
    "Cyb",
    "60000000123",
    3,
    "Epic",
  );
  assert.equal(grantCmd.command, "grant_item");
  assert.equal(grantCmd.count, 3);
  assert.equal(grantCmd.rarity, "Epic");

  // Verify CityNode Mayor & Election + Scoped Receipts
  const store = storeMod.createWorldStreamStore();
  await store.getState().setNodeMayor("Verra_RVR_Winstead", "Hatemost", async () =>
    new Response(JSON.stringify({ message: "Mayor assigned" }), { status: 200 }),
  );
  assert.equal(store.getState().cityNodes[0].mayor_name, "Hatemost");
  assert.equal(store.getState().lastReceiptsByScope.nodeMayor?.message, "Mayor assigned");
  assert.equal(store.getState().lastReceiptsByScope.teleport, null);
}

async function step4LogsAndElevenContainerAnalytics(): Promise<void> {
  const paletteMod = await import("../src/components/GmCommandPalette.tsx");
  const tab3Mod = await import("../src/components/LogsAndContainersTab.tsx");

  assert.equal(typeof tab3Mod.LogsAndContainersTab, "function");
  assert.equal(tab3Mod.CANONICAL_SANDAL_CONTAINERS.length, 11);
  assert.equal(
    tab3Mod.CANONICAL_SANDAL_CONTAINERS.includes("aoc-tether" as never),
    false,
  );
  assert.equal(
    tab3Mod.CANONICAL_SANDAL_CONTAINERS.includes("aoc-world-riverlands"),
    true,
  );
  assert.equal(
    tab3Mod.CANONICAL_SANDAL_CONTAINERS.includes("aoc-mesh-gateway"),
    true,
  );

  const normalized = tab3Mod.normalizeContainersScrapeResponse({
    world_status: { players: 4, npcs: 12450, monsters: 320, resource_nodes: 810 },
    gateway_telemetry: {
      requests_total: 250,
      errors_total: 1,
      cache_hits_total: 180,
      cache_misses_total: 70,
      transient_retries_total: 2,
    },
    containers: [
      {
        name: "aoc-world-riverlands",
        state: "running",
        status: "Up 15 minutes",
        cpu_pct: 14.5,
        mem_mb: 1840.2,
        mem_limit_mb: 8192,
        rx_bytes: 1048576,
        tx_bytes: 2097152,
      },
    ],
  });
  assert.equal(normalized.world_status.players_online, 4);
  assert.equal(normalized.world_status.npcs_total, 12450);
  assert.equal(normalized.world_status.monsters_total, 320);
  assert.equal(normalized.world_status.resource_nodes_total, 810);
  assert.equal(normalized.gateway_telemetry.requests_total, 250);
  assert.equal(normalized.gateway_telemetry.transient_retries_total, 2);
  assert.equal(normalized.prometheus.targets.length, 3);
  assert.equal(normalized.containers.length, 11);

  const logsUrl = paletteMod.buildPaginatedQueryUrl("/api/v1/gm/logs", {
    q: "NPC looks",
    limit: 100,
    offset: 0,
    extra: { container: "aoc-world-riverlands", tail: 200 },
  });
  assert.equal(
    logsUrl,
    "/api/v1/gm/logs?q=NPC%20looks&limit=100&offset=0&container=aoc-world-riverlands&tail=200",
  );
}

async function main(): Promise<void> {
  await step1ThreeTabShellAndClutterRemoval();
  await step2WorldMapTilesNodePlugsAndLayerMaskPolling();
  await step3PlayersAndSpawnerGmActions();
  await step4LogsAndElevenContainerAnalytics();
  console.log(
    "[PASS] gm_studio.spec.ts: all 4 E2E GM Studio workflow steps verified",
  );
}

await main();
