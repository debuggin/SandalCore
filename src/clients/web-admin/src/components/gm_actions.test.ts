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
        url: "data:text/javascript,export const useState = (v) => [typeof v === 'function' ? v() : v, () => {}]; export const useEffect = () => {}; export const useMemo = (fn) => fn(); export const useCallback = (fn) => fn; export const useRef = (v) => ({ current: v }); export default { useState, useEffect, useMemo, useCallback, useRef };",
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

async function testVerraCoordinateMathAndHighResTiles(): Promise<void> {
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
  assert.equal(originU, 47 * 512);
  assert.equal(originV, 17 * 512);

  const [roundX, roundY] = storeMod.mosaicPxToWorldCm(originU, originV);
  assert.equal(roundX, 0);
  assert.equal(roundY, 0);

  const sampleCm = { x_cm: -1650000, y_cm: 625000 };
  const [sampleU, sampleV] = storeMod.worldCmToMosaicPx(
    sampleCm.x_cm,
    sampleCm.y_cm,
  );
  const [restoredX, restoredY] = storeMod.mosaicPxToWorldCm(sampleU, sampleV);
  assert.ok(Math.abs(restoredX - sampleCm.x_cm) < 1e-6);
  assert.ok(Math.abs(restoredY - sampleCm.y_cm) < 1e-6);

  assert.equal(storeMod.nodeLevelToPlugTier(0), -1);
  assert.equal(storeMod.nodeLevelToPlugTier(1), 0);
  assert.equal(storeMod.nodeLevelToPlugTier(2), 1);
  assert.equal(storeMod.nodeLevelToPlugTier(3), 2);
  assert.equal(storeMod.nodeLevelToPlugTier(4), 3);
  assert.equal(storeMod.nodeLevelToPlugTier(5), 4);
  assert.equal(storeMod.nodeLevelToPlugTier(6), 4);

  assert.equal(storeMod.DEFAULT_VERRA_CITY_NODES.length, 26);
  const winstead = storeMod.DEFAULT_VERRA_CITY_NODES[0];
  assert.equal(
    storeMod.computeNodePlugTileOverlay({ ...winstead, level: 0 }),
    null,
  );
  const stage3Overlay = storeMod.computeNodePlugTileOverlay({
    ...winstead,
    level: 3,
  });
  assert.ok(stage3Overlay !== null);
  assert.equal(stage3Overlay.plugTier, 2);
  assert.equal(stage3Overlay.tileUrl, "/api/v1/world/tiles/8/0/2");
  assert.equal(stage3Overlay.sizeMosaicPx, 1024);

  // Verify computeVisibleBaseTiles always uses high-detail z=6 or z=7 across full zoom range
  const continentTiles = storeMod.computeVisibleBaseTiles(
    storeMod.DEFAULT_VERRA_MAP_META,
    -777664,
    522023,
    0.0,
    { width: 1280, height: 720 },
  );
  assert.ok(continentTiles.length > 0);
  assert.ok(continentTiles.every((t) => t.z === 6 || t.z === 7));

  const streetTiles = storeMod.computeVisibleBaseTiles(
    storeMod.DEFAULT_VERRA_MAP_META,
    -777664,
    522023,
    10.0,
    { width: 1280, height: 720 },
  );
  assert.ok(streetTiles.length > 0);
  assert.ok(streetTiles.every((t) => t.z === 7));
}

async function testStreamLayerBitmasksAndDynamicBackendPolling(): Promise<void> {
  const storeMod = await import("../state/worldStreamStore.ts");

  assert.equal(storeMod.STREAM_LAYER_PLAYERS, 1);
  assert.equal(storeMod.STREAM_LAYER_NPCS, 2);
  assert.equal(storeMod.STREAM_LAYER_RESOURCE_NODES, 4);
  assert.equal(storeMod.STREAM_LAYER_MOUNTS, 8);
  assert.equal(storeMod.STREAM_LAYER_LOOT_PILES, 16);
  assert.equal(storeMod.STREAM_LAYER_ALL, 31);
  assert.equal(storeMod.LAYER_POIS, 32);
  assert.equal(storeMod.LAYER_MONSTERS, 64);

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
    storeMod.buildActiveLayersParam(defaultLayers),
    "players,npcs,monsters,pois",
  );
  assert.equal(storeMod.shouldPollWorldState(defaultLayers), true);
  assert.equal(storeMod.shouldFetchPoisCatalog(defaultLayers), true);
  assert.equal(
    storeMod.buildWorldStatePollUrl(defaultLayers),
    "/api/v1/world/state?layer_mask=99&layers=players,npcs,monsters,pois",
  );

  // Toggling OFF monsters removes monsters bit (64) and name from ?layers=
  const noMonsters = { ...defaultLayers, monsters: false };
  assert.equal(storeMod.computeStreamLayerMask(noMonsters), 35);
  assert.equal(
    storeMod.buildActiveLayersParam(noMonsters),
    "players,npcs,pois",
  );
  assert.equal(
    storeMod.buildWorldStatePollUrl(noMonsters),
    "/api/v1/world/state?layer_mask=35&layers=players,npcs,pois",
  );

  // Toggling OFF npcs removes npcs bit (2) and name from ?layers=
  const noNpcs = { ...noMonsters, npcs: false };
  assert.equal(storeMod.computeStreamLayerMask(noNpcs), 33);
  assert.equal(storeMod.buildActiveLayersParam(noNpcs), "players,pois");
  assert.equal(
    storeMod.buildWorldStatePollUrl(noNpcs),
    "/api/v1/world/state?layer_mask=33&layers=players,pois",
  );

  // Toggling OFF pois stops /api/v1/catalog/places fetch and removes pois from ?layers=
  const onlyPlayers = { ...noNpcs, pois: false };
  assert.equal(storeMod.shouldFetchPoisCatalog(onlyPlayers), false);
  assert.equal(storeMod.computeStreamLayerMask(onlyPlayers), 1);
  assert.equal(storeMod.buildActiveLayersParam(onlyPlayers), "players");
  assert.equal(
    storeMod.buildWorldStatePollUrl(onlyPlayers),
    "/api/v1/world/state?layer_mask=1&layers=players",
  );

  // Toggling OFF all layers sets layerMask=0 and pauses /api/v1/world/state polling completely
  const allHidden = { ...onlyPlayers, players: false };
  assert.equal(storeMod.computeStreamLayerMask(allHidden), 0);
  assert.equal(storeMod.buildActiveLayersParam(allHidden), "");
  assert.equal(storeMod.shouldPollWorldState(allHidden), false);
  assert.equal(storeMod.shouldFetchPoisCatalog(allHidden), false);
  assert.equal(storeMod.buildWorldStatePollUrl(allHidden), null);

  // Verify proto STREAM_LAYER_ALL (31) when all 5 proto entity layers are enabled
  const protoFive = {
    players: true,
    npcs: true,
    monsters: false,
    resource_nodes: true,
    mounts: true,
    loot_piles: true,
    pois: false,
  };
  assert.equal(
    storeMod.computeStreamLayerMask(protoFive),
    storeMod.STREAM_LAYER_ALL,
  );
}

async function testGmCommandBuildersPaginationAndTransientRetry(): Promise<void> {
  const paletteMod = await import("./GmCommandPalette.tsx");

  const tpPlace = paletteMod.buildTeleportPlayerCommand({
    character: "Aurelia",
    place: "Lionhold",
  });
  assert.equal(tpPlace.command, "teleport_player");
  assert.equal(tpPlace.character, "Aurelia");
  assert.equal("place" in tpPlace ? tpPlace.place : "", "Lionhold");

  const tpCoords = paletteMod.buildTeleportPlayerCommand({
    character: "Aurelia",
    x_cm: -1650000,
    y_cm: 625000,
    z_cm: 12500,
  });
  assert.equal("x_cm" in tpCoords ? tpCoords.x_cm : 0, -1650000);
  assert.equal("y_cm" in tpCoords ? tpCoords.y_cm : 0, 625000);
  assert.equal("z_cm" in tpCoords ? tpCoords.z_cm : 0, 12500);

  const moneyCmd = paletteMod.buildGiveMoneyCommand("Aurelia", 10, 25, 50);
  assert.equal(moneyCmd.command, "give_money");
  assert.equal(moneyCmd.gold, 10);
  assert.equal(moneyCmd.silver, 25);
  assert.equal(moneyCmd.copper, 50);

  const itemCmd = paletteMod.buildGrantItemCommand(
    "Aurelia",
    "60000000001",
    5,
    "Legendary",
  );
  assert.equal(itemCmd.command, "grant_item");
  assert.equal(itemCmd.count, 5);
  assert.equal(itemCmd.rarity, "Legendary");

  const spawnCmd = paletteMod.buildSpawnNpcCommand({
    npc: "40000000009",
    level: 35,
    x_cm: 1000,
    y_cm: 2000,
    z_cm: 300,
  });
  assert.equal(spawnCmd.command, "spawn_npc");
  assert.equal(spawnCmd.level, 35);

  const nodeCmd = paletteMod.buildSetNodeLevelCommand("Winstead", 4);
  assert.equal(nodeCmd.command, "set_node_level");
  assert.equal(nodeCmd.level, 4);
  assert.equal(nodeCmd.plug_tier, 3);

  const queryUrl = paletteMod.buildPaginatedQueryUrl("/api/v1/catalog/items", {
    q: "sword",
    limit: 25,
    offset: 50,
  });
  assert.equal(queryUrl, "/api/v1/catalog/items?q=sword&limit=25&offset=50");

  const env = paletteMod.normalizePaginatedEnvelope<{ id: string }>(
    { items: [{ id: "1" }], total: 80, limit: 25, offset: 50, has_more: true },
    ["items"],
    25,
    50,
  );
  assert.equal(env.total, 80);
  assert.equal(env.has_more, true);

  const problem = paletteMod.parseRfc9457Problem(422, {
    title: "Invalid GM Command",
    detail: "Unknown target player",
  });
  assert.equal(problem.title, "Invalid GM Command");
  assert.equal(problem.detail, "Unknown target player");

  // Test transient retry wrapper via sendGmCommand (503 -> 200)
  let attempts = 0;
  const mockFetch: typeof fetch = async (_input, init) => {
    attempts += 1;
    const headers = init?.headers as Record<string, string>;
    assert.ok(headers["X-Correlation-ID"]?.length === 36);
    assert.ok(headers["Idempotency-Key"]?.length === 36);
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
}

async function testGmRbacSpatialPayloadsAndInspector(): Promise<void> {
  const paletteMod = await import("./GmCommandPalette.tsx");
  const menuMod = await import("./WorldContextMenu.tsx");
  const inspectorMod = await import("./EntityInspector.tsx");

  assert.equal(paletteMod.GM_COMMAND_DOMAINS.length, 6);
  assert.equal(
    paletteMod.validateGmCommandRbac("GM_OBSERVER", "Teleport"),
    false,
  );
  assert.equal(
    paletteMod.validateGmCommandRbac("GM_OPERATOR", "Teleport"),
    true,
  );

  const precisePoint = {
    xCm: -345678.123456789,
    yCm: 891234.987654321,
    zCm: 14250.555555555,
  };
  const teleport = menuMod.buildGroundSnappedTeleportPayload(
    precisePoint,
    "char-77",
  );
  assert.equal(teleport.command, "Teleport");
  assert.equal(teleport.x, -345678.123456789);
  assert.equal(teleport.capsule_half_height_cm, 96.0);

  const model = inspectorMod.buildEntityInspectorModel({
    viewState: "populated",
    entity: {
      guid: "npc-9001",
      name: "Corrupted Grizzly",
      kind: "npc",
      level: 45,
      healthPct: 82.5,
      hp: 16500,
      maxHp: 20000,
      xCm: 125000.5,
      yCm: -64000.25,
      zCm: 3200.75,
      stateFlags: ["COMBAT", "ENRAGED"],
      activeBuffs: ["Bloodlust", "Thorns"],
    },
  });
  assert.equal(model.role, "grid");
  assert.equal(model.rows.length, 9);
  assert.equal(inspectorMod.moveInspectorGridRowFocus(0, 9, "ArrowDown"), 1);
  assert.deepEqual(menuMod.navigateContextMenuKey(0, 5, "ArrowDown"), {
    nextIndex: 1,
    dismissed: false,
  });
}

await testVerraCoordinateMathAndHighResTiles();
await testStreamLayerBitmasksAndDynamicBackendPolling();
await testGmCommandBuildersPaginationAndTransientRetry();
await testGmRbacSpatialPayloadsAndInspector();
console.log(
  "PASS: gm_actions unit and layer bitmask contract suite (4/4 groups)",
);
