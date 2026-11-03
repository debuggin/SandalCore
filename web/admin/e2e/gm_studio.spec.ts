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
    if (specifier === "react") {
      return { url: "data:text/javascript,export default {};", shortCircuit: true };
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
      const stripped = raw.replace(
        /return\s*\(\s*<([A-Za-z]+)[\s\S]*?<\/\1>\s*\);/g,
        "return null as any;"
      );
      return nextLoad(url, { ...context, format: "module-typescript", source: stripped });
    }
    return nextLoad(url, context);
  },
});

async function step1SpatialMapPmTilesAndEntitySelection(): Promise<void> {
  const mapMod = await import("../src/map/VerraMapCanvas.tsx");
  const storeMod = await import("../src/state/worldStreamStore.ts");
  const inspectorMod = await import("../src/components/EntityInspector.tsx");

  assert.equal(mapMod.DEFAULT_VERRA_PMTILES_URL, "pmtiles:///tiles/verra.pmtiles");
  let registeredScheme = "";
  const unregister = mapMod.registerPmtilesProtocol(
    (scheme) => {
      registeredScheme = scheme;
    },
    () => {
      registeredScheme = "removed";
    }
  );
  assert.equal(registeredScheme, "pmtiles");
  unregister();
  assert.equal(registeredScheme, "removed");

  const [lng, lat] = storeMod.verraUnrealToLngLat(128000.5, -64000.25);
  const roundTrip = storeMod.lngLatToVerraUnreal(lng, lat, 512.0);
  assert.ok(Math.abs(roundTrip.xCm - 128000.5) < 1e-6);
  assert.ok(Math.abs(roundTrip.yCm - -64000.25) < 1e-6);

  const entities = new Map([
    ["player-1", { guid: "player-1", kind: "player" as const, name: "Cyb", xCm: 100, yCm: 200, zCm: 0, yawDeg: 90 }],
    ["spawn-1", { guid: "spawn-1", kind: "spawn_group" as const, name: "Goblin Camp", xCm: 300, yCm: 400, zCm: 0, yawDeg: 0 }],
  ]);
  const baseLayers = { players: true, npcs: true, spawns: true, navmesh: true, voronoiBorders: true };
  const lodZ1 = mapMod.buildVerraDeckOverlayLayers({
    entities,
    selectedEntityGuid: "player-1",
    layers: baseLayers,
    camera: { longitude: 0, latitude: 0, zoom: 1.5, pitch: 0, bearing: 0, mode: "2d" },
    voronoiZones: [],
    navmeshSegments: [],
    lastSeq: 1,
  });
  const lodZ4 = mapMod.buildVerraDeckOverlayLayers({
    entities,
    selectedEntityGuid: "player-1",
    layers: baseLayers,
    camera: { longitude: 0, latitude: 0, zoom: 2.2, pitch: 0, bearing: 0, mode: "2d" },
    voronoiZones: [],
    navmeshSegments: [],
    lastSeq: 2,
  });
  const lodZ6 = mapMod.buildVerraDeckOverlayLayers({
    entities,
    selectedEntityGuid: "player-1",
    layers: baseLayers,
    camera: { longitude: 0, latitude: 0, zoom: 6.0, pitch: 45, bearing: 0, mode: "3d" },
    voronoiZones: [],
    navmeshSegments: [],
    lastSeq: 3,
  });
  assert.equal(lodZ1[2].data.length, 1);
  assert.equal(lodZ1[3].visible, false);
  assert.equal(lodZ4[2].data.length, 2);
  assert.equal(lodZ4[3].visible, false);
  assert.equal(lodZ6[2].data.length, 2);
  assert.equal(lodZ6[3].visible, true);

  const inspectorModel = inspectorMod.buildEntityInspectorModel({
    entity: {
      ...(entities.get("player-1") ?? { guid: "player-1", kind: "player" as const, name: "Cyb", xCm: 100, yCm: 200, zCm: 0, yawDeg: 90 }),
      hotfixOverlays: [
        { target_table: "world_hotfix_overlays", record_guid: "player-1", field_path: "max_hp", old_value: 1000, new_value: 1500 },
      ],
    },
    viewState: "populated",
  });
  assert.equal(inspectorModel.viewState, "populated");
  assert.equal(inspectorModel.hotfixDiffs.length, 1);
  assert.equal(inspectorModel.hotfixDiffs[0].diffSummary, "1000 -> 1500");
}

async function step2GmCommandPaletteAndContextMenu(): Promise<void> {
  const paletteMod = await import("../src/components/GmCommandPalette.tsx");
  const ctxMod = await import("../src/components/WorldContextMenu.tsx");

  const results = paletteMod.filterGmCommandPalette("Teleport");
  assert.ok(results.length >= 1);
  assert.equal(results[0].command, "Teleport");

  assert.equal(paletteMod.validateGmCommandRbac("GM_OBSERVER", "Teleport"), false);
  assert.equal(paletteMod.validateGmCommandRbac("GM_OPERATOR", "SpawnNpc"), true);
  assert.equal(paletteMod.validateGmCommandRbac("GM_OPERATOR", "TriggerSiege"), false);
  assert.equal(paletteMod.validateGmCommandRbac("GM_ARCHITECT", "TriggerSiege"), true);

  const tp = ctxMod.buildGroundSnappedTeleportPayload({ xCm: 14500.25, yCm: -8200.5, zCm: 0 }, "entity:player:77");
  assert.equal(tp.has_z, false);
  assert.equal(tp.capsule_half_height_cm, 96.0);

  const move = ctxMod.buildMoveNpcPayload("entity:npc:42", { xCm: 1000, yCm: 2000, zCm: 300 });
  assert.equal(move.update_spawn_anchor, true);

  const los = ctxMod.buildRaycastLosPayload(
    { xCm: 0, yCm: 0, zCm: 100 },
    { xCm: 1000, yCm: 0, zCm: 100 },
    { hit_t: 0.42, occluded: true, material_mask: 0x0040010d }
  );
  assert.equal(los.occluded, true);
  assert.ok(los.hit_t >= 0 && los.hit_t <= 1);
}

async function step3HotfixSqlDryRunDdlRejectionAndApply(): Promise<void> {
  const hotfixMod = await import("../src/components/HotfixEditor.tsx");

  assert.equal(hotfixMod.validateClientHotfixSql("DROP TABLE world_hotfix_overlays;").valid, false);
  assert.equal(hotfixMod.validateClientHotfixSql("ALTER TABLE spawns ADD COLUMN x INT;").valid, false);
  assert.equal(hotfixMod.validateClientHotfixSql("PRAGMA journal_mode=WAL;").valid, false);
  assert.equal(hotfixMod.validateClientHotfixSql("DELETE FROM accounts;").valid, false);

  const validSql =
    "UPDATE world_hotfix_overlays SET payload_json = '{\"hp\":2500}' WHERE overlay_key = 'npc:wolf_alpha';";
  const check = hotfixMod.validateClientHotfixSql(validSql);
  assert.equal(check.valid, true);
  assert.equal(check.targetTable, "world_hotfix_overlays");

  const preview = hotfixMod.buildHotfixDryRunPreview({
    sql: validSql,
    operatorRole: "GM_ARCHITECT",
    snapshotEpoch: 4,
    existingValue: "{\"hp\":1800}",
    sequence: 10,
  });
  assert.equal(preview.valid, true);
  assert.equal(preview.viewState, "populated");
  assert.equal(preview.canApply, true);
  assert.ok(preview.explainPlan.length >= 3);
  assert.equal(preview.rowDiffs[0].oldValue, "{\"hp\":1800}");
  assert.equal(preview.rowDiffs[0].newValue, "{\"hp\":2500}");
  assert.ok(preview.generatedUndoSql.includes("UPDATE world_hotfix_overlays"));

  const applyEnvelope = hotfixMod.buildHotfixHttpRequest("apply", {
    sql: validSql,
    expectedUndoSql: preview.generatedUndoSql,
    idempotencyKey: preview.idempotencyKey,
  });
  assert.equal(applyEnvelope.url, "/api/v1/gm/hotfix/apply");
  assert.equal(applyEnvelope.headers["Idempotency-Key"], preview.idempotencyKey);
  assert.equal(applyEnvelope.headers["X-Idempotency-Key"], preview.idempotencyKey);
}

async function step4AuditLogInspectionAndOneClickRollback(): Promise<void> {
  const auditMod = await import("../src/components/AuditRollback.tsx");

  const sampleEntry = {
    auditId: "audit-000001",
    actorSub: "gm.architect",
    actorRole: "GM_ARCHITECT",
    commandDomain: "HOTFIX_SQL",
    actionName: "HOTFIX_APPLY",
    targetEntityId: "world_hotfix_overlays:npc:wolf_alpha",
    undoSql: "UPDATE world_hotfix_overlays SET payload_json = '{\"hp\":1800}' WHERE overlay_key = 'npc:wolf_alpha';",
    rolledBack: false,
    timestampIso: "2026-11-03T21:00:00Z",
    snapshotEpoch: 5,
  };

  assert.equal(auditMod.resolveAuditViewState({ entries: [], isLoading: true }), "loading");
  assert.equal(auditMod.resolveAuditViewState({ entries: [] }), "empty");
  assert.equal(auditMod.resolveAuditViewState({ entries: [], errorMessage: "Stream error" }), "error");
  assert.equal(auditMod.resolveAuditViewState({ entries: [sampleEntry], isDegraded: true }), "degraded");
  assert.equal(auditMod.resolveAuditViewState({ entries: [sampleEntry] }), "populated");

  assert.equal(auditMod.canRollbackAuditEntry(sampleEntry, "GM_OBSERVER"), false);
  assert.equal(auditMod.canRollbackAuditEntry(sampleEntry, "GM_OPERATOR"), false);
  assert.throws(() => auditMod.buildAuditRollbackRequest(sampleEntry, "GM_OPERATOR"));

  assert.equal(auditMod.canRollbackAuditEntry(sampleEntry, "GM_ARCHITECT"), true);
  const req = auditMod.buildAuditRollbackRequest(sampleEntry, "GM_ARCHITECT");
  assert.equal(req.url, "/api/v1/gm/audit/rollback");
  assert.ok(req.headers["Idempotency-Key"].length === 36);

  const nextEntries = auditMod.applyOptimisticAuditRollback(
    [sampleEntry],
    "audit-000001",
    "audit-000002",
    6
  );
  assert.equal(nextEntries.length, 2);
  assert.equal(nextEntries[0].auditId, "audit-000002");
  assert.equal(nextEntries[0].snapshotEpoch, 6);
  assert.equal(nextEntries[1].rolledBack, true);
  assert.equal(auditMod.canRollbackAuditEntry(nextEntries[1], "GM_ARCHITECT"), false);
  assert.throws(() =>
    auditMod.applyOptimisticAuditRollback(nextEntries, "audit-000001", "audit-000003", 7)
  );
}

async function main(): Promise<void> {
  await step1SpatialMapPmTilesAndEntitySelection();
  await step2GmCommandPaletteAndContextMenu();
  await step3HotfixSqlDryRunDdlRejectionAndApply();
  await step4AuditLogInspectionAndOneClickRollback();
  console.log("[PASS] gm_studio.spec.ts: all 4 E2E GM Studio workflow steps verified");
}

await main();
