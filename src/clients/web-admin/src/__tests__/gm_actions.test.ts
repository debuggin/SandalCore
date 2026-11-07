// SPDX-License-Identifier: AGPL-3.0-only

import assert from "node:assert/strict";

await import("../components/gm_actions.test.ts");
const {
  computeLayerEntityCounts,
  computeNodePlugTileOverlay,
  computeVisibleBaseTiles,
  createWorldStreamStore,
  DEFAULT_VERRA_CITY_NODES,
  DEFAULT_VERRA_MAP_META,
  loadPersistedWebAdminState,
  savePersistedWebAdminState,
} = await import("../state/worldStreamStore.ts");

function verifyEntrypointInvariants(): void {
  const store = createWorldStreamStore();
  store.getState().setPickedCoordinate({ xCm: -641389, yCm: 377367, zCm: 12500 });
  store.getState().selectEntity("npc-42");
  store.getState().selectCityNode("Verra_RVR_Winstead");
  store.getState().clearMapSelection();
  assert.equal(store.getState().lastClickedPoint, null);
  assert.equal(store.getState().selectedEntityGuid, null);
  assert.equal(store.getState().selectedCityNodeName, null);

  store.getState().setScopedReceipt("nodeLevel", {
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
  assert.equal(store.getState().lastReceiptsByScope.entityAction, null);

  const lowZoomTiles = computeVisibleBaseTiles(
    DEFAULT_VERRA_MAP_META,
    -777664,
    522023,
    0.0,
    { width: 1280, height: 720 },
  );
  assert.ok(lowZoomTiles.length > 0 && lowZoomTiles.every((t) => t.z === 3));
  const plug = computeNodePlugTileOverlay(
    { ...DEFAULT_VERRA_CITY_NODES[0], level: 3 },
    5.0,
  );
  assert.ok(
    plug !== null &&
      plug.cxCm === DEFAULT_VERRA_CITY_NODES[0].ox &&
      plug.worldSpanCm === 100000,
  );
  assert.equal(computeLayerEntityCounts(store.getState()).pois, 26);

  const mem = new Map<string, string>();
  const stub = {
    getItem: (k: string) => mem.get(k) ?? null,
    setItem: (k: string, v: string) => {
      mem.set(k, v);
    },
  };
  store.getState().setActiveModal("spawn-npc");
  savePersistedWebAdminState(store.getState(), stub);
  const restored = loadPersistedWebAdminState(
    stub,
    "#tab=analytics&modal=spawn-npc",
  );
  assert.equal(restored.activeTab, "analytics");
  assert.equal(restored.activeModal, "spawn-npc");
}

if (process.env.VITEST) {
  const { describe, it } = await import("vitest");
  describe("gm_actions __tests__ entrypoint", () => {
    it("verifies store selection, tile overlay, and persistence invariants", () => {
      verifyEntrypointInvariants();
    });
  });
} else {
  verifyEntrypointInvariants();
}
