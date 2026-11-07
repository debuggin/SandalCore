import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import {
  computeLiveForwardedFrames,
  computeMutuallyExclusivePopulation,
  EMPTY_WORLD_STATUS,
  formatScopeTierBadge,
  LOG_TAIL_OPTIONS,
  normalizeContainersScrapeResponse,
} from "../components/LogsAndContainersTab";
import { classifyWorldEntityCategory } from "../map/verraTilePyramidRenderer";
import {
  createWorldStreamStore,
  DEFAULT_REGIONAL_WORLD_WORKERS,
  formatWorkerTickHzOptionLabel,
  resolveWorkerTickFields,
  type WorldEntity,
  type WorldWorkerInfo,
} from "../state/worldStreamStore";

describe("Observability & LogsAndContainersTab (Wave 42C-2)", () => {
  it("formatScopeTierBadge returns clean scope tier label without Hz suffix and Gateway Errors / Retries renders cleanly", () => {
    expect(formatScopeTierBadge("world")).toBe("WORLD");
    expect(formatScopeTierBadge("region")).toBe("REGION");
    expect(formatScopeTierBadge("node_plug")).toBe("NODE_PLUG");
    expect(formatScopeTierBadge("polygon_cutout")).toBe("POLYGON_CUTOUT");
    expect(formatScopeTierBadge("zoi")).toBe("ZOI");

    const logsSrc = readFileSync(
      fileURLToPath(new URL("../components/LogsAndContainersTab.tsx", import.meta.url)),
      "utf8"
    );
    expect(logsSrc.includes("`Retries: ${gt.transient_retries_total}`")).toBe(false);
    expect(
      logsSrc.includes(
        'renderMetricItem("metric-errors-total", "Gateway Errors / Retries", `${gt.errors_total} / ${gt.transient_retries_total}`)'
      )
    ).toBe(true);
    expect(logsSrc.includes('"Scope Tier & Hz"')).toBe(false);
    expect(logsSrc.includes('"Scope Tier"')).toBe(true);
    expect(logsSrc.includes('"Target Tick Mode"')).toBe(true);
    expect(logsSrc.includes('"Current Hz"')).toBe(true);
    expect(logsSrc.includes("ServerTimeOfDayControl")).toBe(false);
    expect(logsSrc.includes("Server Clock (9x)")).toBe(false);
    expect(logsSrc.includes("`Tail: ${")).toBe(false);
    expect(logsSrc.includes("Follow: ${")).toBe(false);
    expect(logsSrc.includes("[{row.component}]")).toBe(false);

    const snapWorkers = normalizeContainersScrapeResponse({
      workers: [
        { worker_id: "world-riverlands", scope_kind: "region", scope_key: "riverlands", tick_hz: 45, target_tick_hz: 40 },
      ],
    });
    expect(snapWorkers.workers[0].scope_tier_badge).toBe("REGION");
    expect(snapWorkers.workers[0].scope_tier_badge.includes("40Hz")).toBe(false);
    expect(snapWorkers.workers[0].target_tick_hz).toBe(-1);
    expect(snapWorkers.workers[0].tick_mode).toBe("dynamic");
    expect(snapWorkers.workers[0].current_tick_hz).toBe(45);
  });

  it("separates target_tick_hz (dropdown setting) from current_tick_hz (live output)", () => {
    for (const defWorker of DEFAULT_REGIONAL_WORLD_WORKERS) {
      expect(defWorker.target_tick_hz).toBe(-1);
      expect(defWorker.tick_mode).toBe("dynamic");
      expect(defWorker.current_tick_hz).toBeGreaterThanOrEqual(30);
    }

    const baseWorker: WorldWorkerInfo = {
      worker_id: "world-riverlands", canonical_name: "aoc-riverlands", container_name: "aoc-world-riverlands",
      scope_key: "riverlands", scope_kind: "region", region_label: "Riverlands", status: "UP",
      tick_hz: 48, current_tick_hz: 48, target_tick_hz: -1, tick_mode: "dynamic",
    };

    const dynamicResolved = resolveWorkerTickFields(baseWorker);
    expect(dynamicResolved.target_tick_hz).toBe(-1);
    expect(dynamicResolved.current_tick_hz).toBe(48);
    expect(dynamicResolved.tick_mode).toBe("dynamic");

    const unannotatedFortyFiveResolved = resolveWorkerTickFields({
      ...baseWorker, tick_hz: 45, current_tick_hz: 45, target_tick_hz: 45, tick_mode: undefined,
    });
    expect(unannotatedFortyFiveResolved.target_tick_hz).toBe(-1);
    expect(unannotatedFortyFiveResolved.current_tick_hz).toBe(45);
    expect(unannotatedFortyFiveResolved.tick_mode).toBe("dynamic");

    const manualResolved = resolveWorkerTickFields({
      ...baseWorker, tick_hz: 60, current_tick_hz: 60, target_tick_hz: 60, tick_mode: "manual",
    });
    expect(manualResolved.target_tick_hz).toBe(60);
    expect(manualResolved.current_tick_hz).toBe(60);
    expect(manualResolved.tick_mode).toBe("manual");

    expect(formatWorkerTickHzOptionLabel(-1, true)).toBe("Dynamic");
    expect(formatWorkerTickHzOptionLabel(-1, false)).toBe("Dynamic (30-60 Hz)");
    expect(formatWorkerTickHzOptionLabel(0, false)).toBe("0 Hz (Pause NPCs)");
    expect(formatWorkerTickHzOptionLabel(60, false)).toBe("60 Hz");

    const mapSrc = readFileSync(fileURLToPath(new URL("../map/VerraMapCanvas.tsx", import.meta.url)), "utf8");
    const logsSrc = readFileSync(fileURLToPath(new URL("../components/LogsAndContainersTab.tsx", import.meta.url)), "utf8");
    expect(mapSrc).toContain('aria-readonly="true"');
    expect(mapSrc).toContain("{currentHz} Hz");
    expect(logsSrc).toContain('aria-readonly="true"');
    expect(logsSrc).toContain("{currentHz} Hz");
  });

  it("computeLiveForwardedFrames derives non-zero live traffic from snapshot telemetry", () => {
    const snapDirect = normalizeContainersScrapeResponse({
      prometheus: {
        mesh_gateway_metrics: {
          udp_frames_rx_total: 800,
          udp_frames_tx_total: 450,
        },
      },
    });
    expect(computeLiveForwardedFrames(snapDirect)).toBe(1250);

    const snapFallback = normalizeContainersScrapeResponse({
      gateway_telemetry: { requests_total: 42 },
      containers: [{ name: "aoc-world-riverlands", state: "running", rx_bytes: 65536, tx_bytes: 32768 }],
    });
    expect(computeLiveForwardedFrames(snapFallback)).toBeGreaterThan(0);
  });

  it("executeSetWorkerTickRateAction preserves target_tick_hz when switching between Dynamic and Manual", async () => {
    const store = createWorldStreamStore({ workers: DEFAULT_REGIONAL_WORLD_WORKERS });
    const calls: string[] = [];
    const mockFetch: typeof fetch = async (_input, init) => {
      calls.push(String(init?.body ?? ""));
      return new Response(JSON.stringify({ status: "ok" }), { status: 200 });
    };

    await store.getState().executeSetWorkerTickRateAction("world-riverlands", 60, mockFetch);
    const manualWorker = store.getState().workers.find((w: WorldWorkerInfo) => w.worker_id === "world-riverlands");
    expect(manualWorker?.target_tick_hz).toBe(60);
    expect(manualWorker?.current_tick_hz).toBe(60);
    expect(manualWorker?.tick_mode).toBe("manual");

    await store.getState().executeSetWorkerTickRateAction("world-riverlands", -1, mockFetch);
    const dynamicWorker = store.getState().workers.find((w: WorldWorkerInfo) => w.worker_id === "world-riverlands");
    expect(dynamicWorker?.target_tick_hz).toBe(-1);
    expect(dynamicWorker?.current_tick_hz).toBeGreaterThanOrEqual(30);
    expect(dynamicWorker?.tick_mode).toBe("dynamic");
    expect(calls.length).toBe(2);
    expect(JSON.parse(calls[1]).tick_hz).toBe(-1);
    expect(JSON.parse(calls[1]).tick_mode).toBe("dynamic");
  });

  it("partitions world population into mutually-exclusive categories (including Huntables, Trees, Rocks, Ore & Gems, Plants) with zero double-counting", () => {
    const sampleEntities: readonly WorldEntity[] = [
      { guid: "p1", name: "Aurelia", kind: "player", xCm: 0, yCm: 0, zCm: 0, yawDeg: 0, level: 25, healthPct: 100 },
      { guid: "n1", name: "Lionhold Guard", kind: "npc", xCm: 0, yCm: 0, zCm: 0, yawDeg: 0, level: 30, healthPct: 100 },
      { guid: "m1", name: "Riverlands Wolf", kind: "monster", xCm: 0, yCm: 0, zCm: 0, yawDeg: 0, level: 12, healthPct: 100 },
      { guid: "h1", name: "Hunting_Greathorn_Stag", kind: "monster", npcSubkind: "huntable", xCm: 0, yCm: 0, zCm: 0, yawDeg: 0, level: 10, healthPct: 100 },
      { guid: "h2", name: "Wild Ram", kind: "resource_node", resourceType: "Hunting / Husbandry", xCm: 0, yCm: 0, zCm: 0, yawDeg: 0, level: 8, healthPct: 100 },
      { guid: "t1", name: "Weeping Willow", kind: "resource_node", resourceType: "Lumberjacking / Wood", xCm: 0, yCm: 0, zCm: 0, yawDeg: 0, level: 1, healthPct: 100 },
      { guid: "r1", name: "Basalt Outcrop", kind: "resource_node", resourceType: "Stonecutting / Granite", xCm: 0, yCm: 0, zCm: 0, yawDeg: 0, level: 1, healthPct: 100 },
      { guid: "o1", name: "Rubidium Ore Vein", kind: "resource_node", resourceType: "Mining / Ruby Geode", xCm: 0, yCm: 0, zCm: 0, yawDeg: 0, level: 1, healthPct: 100 },
      { guid: "f1", name: "Spiritbloom Patch", kind: "resource_node", resourceType: "Herbalism / Plant", xCm: 0, yCm: 0, zCm: 0, yawDeg: 0, level: 1, healthPct: 100 },
      { guid: "mt1", name: "Armored Destrier", kind: "mount", xCm: 0, yCm: 0, zCm: 0, yawDeg: 0, level: 1, healthPct: 100 },
    ];

    expect(sampleEntities.map((e) => classifyWorldEntityCategory(e))).toEqual([
      "players", "npcs", "monsters", "huntables", "huntables", "trees", "rocks", "ore_gems", "plants", "mounts",
    ]);

    const entityMap = new Map(sampleEntities.map((e) => [e.guid, e]));
    const pop = computeMutuallyExclusivePopulation(EMPTY_WORLD_STATUS, entityMap);
    expect([pop.players_online, pop.npcs_total, pop.monsters_total, pop.huntables_total, pop.trees_total, pop.rocks_total, pop.ore_gems_total, pop.plants_total, pop.mounts_total, pop.resource_nodes_total]).toEqual([1, 1, 1, 2, 1, 1, 1, 1, 1, 4]);
    const categorySum = pop.players_online + pop.npcs_total + pop.monsters_total + pop.huntables_total + pop.trees_total + pop.rocks_total + pop.ore_gems_total + pop.plants_total + pop.mounts_total;
    expect(categorySum).toBe(pop.total_entities);
    expect(pop.total_entities).toBe(sampleEntities.length);

    // Pre-hydration summary deduplication when backend npcs_total included monsters + huntables
    const snapPreHydrated = normalizeContainersScrapeResponse({
      world_status: {
        total_entities: 297, players_online: 5, npcs_total: 200, monsters_total: 45, huntables_total: 15,
        trees_total: 35, rocks_total: 20, ore_gems_total: 15, plants_total: 10, mounts_total: 12,
      },
    });
    expect(snapPreHydrated.world_status.npcs_total).toBe(140);
    const ws = snapPreHydrated.world_status;
    const preSum = ws.players_online + ws.npcs_total + ws.monsters_total + ws.huntables_total + ws.trees_total + ws.rocks_total + ws.ore_gems_total + ws.plants_total + ws.mounts_total;
    expect(preSum).toBe(ws.total_entities);
    expect(ws.total_entities).toBe(297);
  });

  it("renders brief API Cache Hits / Misses, all HTTP status classes (2xx/3xx/4xx/5xx), and human-digestible telemetry prose without Scrape/OTel/Series jargon", () => {
    const snap = normalizeContainersScrapeResponse({
      prometheus: {
        cluster_metrics: {
          gateway_http: {
            cluster: "gateway_http",
            container: "aoc-gateway",
            requests_total: 150,
            responses_3xx_total: 10,
            errors_4xx_total: 5,
            errors_5xx_total: 2,
            timeouts_total: 1,
            active_connections: 8,
            healthy_members: 1,
          },
        },
      },
    });
    const route = snap.prometheus.cluster_metrics.gateway_http;
    expect(route.requests_total).toBe(150);
    expect(route.responses_2xx_total).toBe(133);
    expect(route.responses_3xx_total).toBe(10);
    expect(route.errors_4xx_total).toBe(5);
    expect(route.errors_5xx_total).toBe(2);

    const logsSrc = readFileSync(
      fileURLToPath(new URL("../components/LogsAndContainersTab.tsx", import.meta.url)),
      "utf8"
    );
    expect(logsSrc).toContain('"API Cache Hits / Misses"');
    expect(logsSrc).toContain('"Monitored Metrics"');
    expect(logsSrc).toContain('"Processes"');
    expect(logsSrc).toContain('"Health (Status / Metrics / Latency)"');
    expect(logsSrc).toContain('"Huntables"');
    expect(logsSrc).toContain('"Trees"');
    expect(logsSrc).toContain('"Rocks"');
    expect(logsSrc).toContain('"Ore & Gems"');
    expect(logsSrc).toContain('"Plants"');
    expect(logsSrc).toContain("2xx:");
    expect(logsSrc).toContain("3xx:");
    expect(logsSrc).toContain("4xx:");
    expect(logsSrc).toContain("5xx:");
    expect(logsSrc.includes("Metric Families / Series")).toBe(false);
    expect(logsSrc.includes("Metrics Scrape")).toBe(false);
  });

  it("includes 250 in LOG_TAIL_OPTIONS, omits duplicate line counts from Log Explorer title, and omits duplicate preset pills in TeleportCard", () => {
    expect(LOG_TAIL_OPTIONS).toContain(250);
    expect([...LOG_TAIL_OPTIONS]).toEqual([100, 200, 250, 500]);

    const logsSrc = readFileSync(
      fileURLToPath(new URL("../components/LogsAndContainersTab.tsx", import.meta.url)),
      "utf8",
    );
    expect(logsSrc).toContain("const title = `Log Explorer (${props.selectedContainer})`;");
    expect(logsSrc.includes("(${filteredRows.length} lines)")).toBe(false);
    expect(logsSrc).toContain("`${filteredRows.length} / ${props.logEnv.total} Lines`");

    const playersSrc = readFileSync(
      fileURLToPath(new URL("../components/PlayersAndSpawnerTab.tsx", import.meta.url)),
      "utf8",
    );
    expect(playersSrc.includes("presets={placeOpts.slice(0, 4)}")).toBe(false);
    expect(playersSrc).toContain('testIdPrefix="teleport-place"');
  });
});
