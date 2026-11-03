import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { registerHooks } from "node:module";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

// Registers in-memory Node module hooks so .tsx modules strip JSX return trees without node_modules.
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
      const stripped = raw.replace(/return\s*\(\s*<([A-Za-z]+)[\s\S]*?<\/\1>\s*\);/g, "return null as any;");
      return nextLoad(url, { ...context, format: "module-typescript", source: stripped });
    }
    return nextLoad(url, context);
  },
});

async function testGmCommandDomainsAndRbac(): Promise<void> {
  const paletteMod = await import("./GmCommandPalette.tsx");
  assert.equal(paletteMod.GM_COMMAND_DOMAINS.length, 6);
  assert.deepEqual(paletteMod.GM_COMMAND_DOMAINS, [
    "Session/Travel",
    "NPC/Spawns",
    "Progression/Economy",
    "World State",
    "Moderation",
    "Diagnostics",
  ]);
  const expectedCommands = [
    "Teleport", "Summon", "KickPlayer",
    "SpawnNpc", "KillNpc", "RespawnNpc", "ResetNpc", "HealNpc", "DespawnNpc", "MoveNpc",
    "SetLevel", "GrantExperience", "AddItem", "AddCurrency",
    "SetNodeStage", "TriggerSiege", "SetCaravanState", "SetWeather",
    "Ban", "Mute", "Freeze",
    "InspectNavTile", "RaycastLos", "DumpEntityState",
  ];
  assert.equal(paletteMod.GM_COMMAND_CATALOG.length, expectedCommands.length);
  assert.deepEqual(paletteMod.GM_COMMAND_CATALOG.map((c: { name: string }) => c.name), expectedCommands);

  assert.equal(paletteMod.validateGmCommandRbac("GM_OBSERVER", "RaycastLos"), true);
  assert.equal(paletteMod.validateGmCommandRbac("GM_OBSERVER", "Teleport"), false);
  assert.equal(paletteMod.validateGmCommandRbac("GM_OPERATOR", "Teleport"), true);
  assert.equal(paletteMod.validateGmCommandRbac("GM_OPERATOR", "MoveNpc"), false);
  assert.equal(paletteMod.validateGmCommandRbac("GM_ARCHITECT", "MoveNpc"), true);
  assert.equal(paletteMod.validateGmCommandRbac("GM_ARCHITECT", "TriggerSiege"), true);

  const req = paletteMod.buildGmCommandRequest({
    command: "Teleport",
    role: "GM_OPERATOR",
    timestampMs: 1730668800123,
    sequence: 42,
    targetGuid: "player-001",
    payload: { x: 100.5, y: -200.25, has_z: false },
  });
  assert.equal(req.endpoint, "/api/v1/gm/command");
  assert.equal(req.method, "POST");
  assert.match(req.headers["Idempotency-Key"], /^[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
  assert.equal(req.headers["Idempotency-Key"], req.headers["X-Idempotency-Key"]);
  assert.throws(() => paletteMod.buildGmCommandRequest({
    command: "Ban",
    role: "GM_OBSERVER",
    timestampMs: 1730668800123,
    sequence: 1,
  }), /not authorized/);
}

async function testSpatialPayloadsAndFloat64Bounds(): Promise<void> {
  const menuMod = await import("./WorldContextMenu.tsx");
  const precisePoint = {
    xCm: -345678.123456789,
    yCm: 891234.987654321,
    zCm: 14250.555555555,
  };

  const teleport = menuMod.buildGroundSnappedTeleportPayload(precisePoint, "char-77");
  assert.equal(teleport.command, "Teleport");
  assert.equal(teleport.has_z, false);
  assert.equal(teleport.x, -345678.123456789);
  assert.equal(teleport.y, 891234.987654321);
  assert.equal(teleport.capsule_half_height_cm, 96.0);
  assert.equal(teleport.snap_formula, "ground_at(x, y) + kCapsuleHalfHeightCm");

  const moveNpc = menuMod.buildMoveNpcPayload("npc-ash-drake-01", precisePoint);
  assert.equal(moveNpc.command, "MoveNpc");
  assert.equal(moveNpc.npc_guid, "npc-ash-drake-01");
  assert.equal(moveNpc.home_x, -345678.123456789);
  assert.equal(moveNpc.home_y, 891234.987654321);
  assert.equal(moveNpc.home_z, 14250.555555555);
  assert.equal(moveNpc.update_spawn_anchor, true);

  const spawnNpc = menuMod.buildSpawnNpcPayload("tmpl_ember_wolf", precisePoint, 55);
  assert.equal(spawnNpc.x, precisePoint.xCm);
  assert.equal(spawnNpc.z, precisePoint.zCm);
  assert.equal(spawnNpc.level, 55);

  const raycast = menuMod.buildRaycastLosPayload(
    { xCm: 10.25, yCm: 20.5, zCm: 180.75 },
    precisePoint,
    { hit_t: 0.625, occluded: true, material_mask: 0x04 },
  );
  assert.equal(raycast.command, "RaycastLos");
  assert.equal(raycast.target_x, precisePoint.xCm);
  assert.equal(raycast.hit_t, 0.625);
  assert.equal(raycast.occluded, true);
  assert.equal(raycast.material_mask, 0x04);

  assert.throws(() => menuMod.buildGroundSnappedTeleportPayload({ xCm: Number.NaN, yCm: 0, zCm: 0 }), RangeError);
  assert.throws(() => menuMod.buildMoveNpcPayload("npc-1", { xCm: 0, yCm: Number.POSITIVE_INFINITY, zCm: 0 }), RangeError);
  assert.throws(() => menuMod.buildRaycastLosPayload(precisePoint, precisePoint, { hit_t: 1.5 }), RangeError);
}

async function testEntityInspectorGridAndHotfixDiffs(): Promise<void> {
  const inspectorMod = await import("./EntityInspector.tsx");
  assert.ok(inspectorMod.INSPECTOR_CONTRAST_RATIO_AA >= 4.5);

  for (const state of ["loading", "empty", "error", "degraded", "populated"] as const) {
    const model = inspectorMod.buildEntityInspectorModel({
      viewState: state,
      entity: state === "populated" || state === "degraded" ? {
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
        hotfixOverlays: [
          { target_table: "npc_spawns", record_guid: "npc-9001", field_path: "home_x", old_value: "120000.0", new_value: "125000.5" },
          { target_table: "npc_spawns", record_guid: "npc-9001", field_path: "home_x", old_value: "125000.5", new_value: "125000.5" },
        ],
      } : null,
    });
    assert.equal(model.role, "grid");
    assert.equal(model.viewState, state);
    if (state === "populated") {
      assert.equal(model.rows.length, 9);
      assert.equal(model.hotfixDiffs.length, 2);
      assert.equal(model.hotfixDiffs[0].changed, true);
      assert.equal(model.hotfixDiffs[0].summary, "120000.0 -> 125000.5");
      assert.equal(model.hotfixDiffs[1].changed, false);
    }
  }
  assert.equal(inspectorMod.moveInspectorGridRowFocus(0, 9, "ArrowDown"), 1);
  assert.equal(inspectorMod.moveInspectorGridRowFocus(3, 9, "End"), 8);
}

async function testKeyboardNavigationAndFuzzyPalette(): Promise<void> {
  const menuMod = await import("./WorldContextMenu.tsx");
  const paletteMod = await import("./GmCommandPalette.tsx");

  assert.deepEqual(menuMod.navigateContextMenuKey(0, 5, "ArrowDown"), { nextIndex: 1, dismissed: false });
  assert.deepEqual(menuMod.navigateContextMenuKey(0, 5, "ArrowUp"), { nextIndex: 4, dismissed: false });
  assert.deepEqual(menuMod.navigateContextMenuKey(2, 5, "Escape"), { nextIndex: 2, dismissed: true });

  const filtered = paletteMod.filterGmCommandPalette("bvh", [
    { id: "probe_nav_01", label: "BVH Navmesh Probe", category: "node", command: "RaycastLos" },
  ]);
  assert.ok(filtered.length >= 2);
  assert.equal(filtered[0].command, "RaycastLos");

  const mapMod = await import("../map/VerraMapCanvas.tsx");
  assert.equal(typeof mapMod.VerraMapCanvas, "function");
}

await testGmCommandDomainsAndRbac();
await testSpatialPayloadsAndFloat64Bounds();
await testEntityInspectorGridAndHotfixDiffs();
await testKeyboardNavigationAndFuzzyPalette();
console.log("PASS: gm_actions unit and accessibility contract suite (4/4 groups)");
