// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import {
  buildAdminAuthHeaders,
  getRetryResetEpoch,
  handleUnauthorizedStatus,
  registerPendingRetryTimer,
  RETRY_MAX_DELAY_MS,
  subscribeRetryReset,
} from "../state/adminAuthClient";
import { nodeLevelToPlugTier } from "../state/worldStreamStore";

export type GmCommandDomain =
  | "Session/Travel"
  | "NPC/Spawns"
  | "Progression/Economy"
  | "World State"
  | "Moderation"
  | "Diagnostics";

export type GmCommandName =
  | "Teleport"
  | "Summon"
  | "KickPlayer"
  | "SpawnNpc"
  | "KillNpc"
  | "RespawnNpc"
  | "ResetNpc"
  | "HealNpc"
  | "DespawnNpc"
  | "MoveNpc"
  | "SetLevel"
  | "GrantExperience"
  | "AddItem"
  | "AddCurrency"
  | "SetNodeStage"
  | "TriggerSiege"
  | "SetCaravanState"
  | "SetWeather"
  | "Ban"
  | "Mute"
  | "Freeze"
  | "InspectNavTile"
  | "RaycastLos"
  | "DumpEntityState";

export type GmRole = "GM_OBSERVER" | "GM_OPERATOR" | "GM_ARCHITECT";
export type ItemRarity = "Common" | "Uncommon" | "Rare" | "Heroic" | "Epic" | "Legendary";

export const ITEM_RARITIES: readonly ItemRarity[] = Object.freeze([
  "Common",
  "Uncommon",
  "Rare",
  "Heroic",
  "Epic",
  "Legendary",
]);

export interface TeleportToPlayerCommand { readonly command: "teleport_player"; readonly character: string; readonly target_player: string; }
export interface TeleportToPlaceCommand { readonly command: "teleport_player"; readonly character: string; readonly place: string; }
export interface TeleportToCoordsCommand { readonly command: "teleport_player"; readonly character: string; readonly x_cm: number; readonly y_cm: number; readonly z_cm: number; }
export type TeleportPlayerCommandPayload = TeleportToPlayerCommand | TeleportToPlaceCommand | TeleportToCoordsCommand;
export interface TeleportPlayerInput { readonly character: string; readonly target_player?: string; readonly place?: string; readonly x_cm?: number; readonly y_cm?: number; readonly z_cm?: number; }
export interface GiveMoneyCommand { readonly command: "give_money"; readonly character: string; readonly gold: number; readonly silver: number; readonly copper: number; }
export interface GrantItemCommand { readonly command: "grant_item"; readonly character: string; readonly item: string; readonly count: number; readonly rarity: ItemRarity; }
export interface SpawnNpcCommandInput { readonly npc: string; readonly level?: number; readonly place?: string; readonly x_cm?: number; readonly y_cm?: number; readonly z_cm?: number; }
export interface SpawnNpcCommand { readonly command: "spawn_npc"; readonly npc: string; readonly place: string; readonly x_cm: number; readonly y_cm: number; readonly z_cm: number; readonly level: number; }
export interface SetNodeLevelCommand { readonly command: "set_node_level"; readonly node: string; readonly level: number; readonly plug_tier: number; }
export interface SetTimeOfDayCommand { readonly command: "set_time_of_day"; readonly hour: number; readonly reason: string; }

export function buildSetTimeOfDayCommand(hour: number, reason = "Operator updated server time of day"): SetTimeOfDayCommand {
  const cleanHour = ((Number.isFinite(hour) ? hour : 12) % 24 + 24) % 24;
  return { command: "set_time_of_day", hour: cleanHour, reason };
}

export type GmActionCommandPayload = TeleportPlayerCommandPayload | GiveMoneyCommand | GrantItemCommand | SpawnNpcCommand | SetNodeLevelCommand | SetTimeOfDayCommand;
export interface PaginatedQueryParams { readonly q?: string; readonly limit?: number; readonly offset?: number; readonly extra?: Readonly<Record<string, string | number>>; }
export interface PaginatedListEnvelope<T> { readonly items: readonly T[]; readonly total: number; readonly limit: number; readonly offset: number; readonly has_more: boolean; readonly next_cursor: string | null; }
export interface Rfc9457ProblemDetails { readonly type: string; readonly title: string; readonly status: number; readonly detail: string; readonly instance?: string; readonly code?: string; }
export interface GmCommandDefinition { readonly name: GmCommandName; readonly domain: GmCommandDomain; readonly minRole: GmRole; readonly description: string; readonly keywords: readonly string[]; }
export interface GmCatalogItem { readonly id: string; readonly label: string; readonly category: "npc" | "item" | "node" | "weather"; readonly command: GmCommandName; }
export interface GmPaletteOption { readonly id: string; readonly command: GmCommandName; readonly domain: GmCommandDomain; readonly minRole: GmRole; readonly label: string; readonly score: number; readonly catalogItemId: string | null; }

export interface GmCommandEnvelope {
  readonly endpoint: "/api/v1/gm/command";
  readonly method: "POST";
  readonly headers: {
    readonly "Content-Type": "application/json";
    readonly "Idempotency-Key": string;
    readonly "X-Idempotency-Key": string;
  };
  readonly body: {
    readonly command_type: GmCommandName;
    readonly domain: GmCommandDomain;
    readonly actor_role: GmRole;
    readonly target_guid: string | null;
    readonly payload: Readonly<Record<string, unknown>>;
  };
}

export interface GmCommandBuildInput {
  readonly command: GmCommandName;
  readonly role: GmRole;
  readonly timestampMs: number;
  readonly sequence: number;
  readonly targetGuid?: string | null;
  readonly payload?: Readonly<Record<string, unknown>>;
}

export interface GmCommandPaletteProps {
  readonly isOpen: boolean;
  readonly query: string;
  readonly role: GmRole;
  readonly activeIndex?: number;
  readonly catalogItems?: readonly GmCatalogItem[];
  readonly onQueryChange?: (nextQuery: string) => void;
  readonly onExecuteCommand?: (command: GmCommandName, catalogItemId: string | null) => void;
  readonly onClose?: () => void;
}

const ROLE_RANK: Readonly<Record<GmRole, number>> = Object.freeze({
  GM_OBSERVER: 1,
  GM_OPERATOR: 2,
  GM_ARCHITECT: 3,
});

export const GM_COMMAND_DOMAINS: readonly GmCommandDomain[] = Object.freeze([
  "Session/Travel",
  "NPC/Spawns",
  "Progression/Economy",
  "World State",
  "Moderation",
  "Diagnostics",
]);

export const GM_COMMAND_CATALOG: readonly GmCommandDefinition[] = Object.freeze([
  { name: "Teleport", domain: "Session/Travel", minRole: "GM_OPERATOR", description: "Ground-snapped or exact coordinate travel", keywords: ["warp", "goto", "coords"] },
  { name: "Summon", domain: "Session/Travel", minRole: "GM_OPERATOR", description: "Summon player to operator position", keywords: ["pull", "bring"] },
  { name: "KickPlayer", domain: "Session/Travel", minRole: "GM_OPERATOR", description: "Disconnect active player session", keywords: ["disconnect", "drop"] },
  { name: "SpawnNpc", domain: "NPC/Spawns", minRole: "GM_OPERATOR", description: "Spawn NPC template at world coordinate", keywords: ["create", "mob", "creature"] },
  { name: "KillNpc", domain: "NPC/Spawns", minRole: "GM_OPERATOR", description: "Force immediate NPC death", keywords: ["slay", "smite"] },
  { name: "RespawnNpc", domain: "NPC/Spawns", minRole: "GM_OPERATOR", description: "Respawn NPC at home anchor", keywords: ["revive", "reset"] },
  { name: "ResetNpc", domain: "NPC/Spawns", minRole: "GM_OPERATOR", description: "Evade and clear NPC threat table", keywords: ["evade", "leashed"] },
  { name: "HealNpc", domain: "NPC/Spawns", minRole: "GM_OPERATOR", description: "Restore NPC full health and mana", keywords: ["restore", "hp"] },
  { name: "DespawnNpc", domain: "NPC/Spawns", minRole: "GM_OPERATOR", description: "Remove spawned NPC instance", keywords: ["delete", "remove"] },
  { name: "MoveNpc", domain: "NPC/Spawns", minRole: "GM_ARCHITECT", description: "Relocate NPC home_x/home_y/home_z anchor", keywords: ["drag", "relocate", "home"] },
  { name: "SetLevel", domain: "Progression/Economy", minRole: "GM_ARCHITECT", description: "Set character combat or artisan level", keywords: ["rank", "lvl"] },
  { name: "GrantExperience", domain: "Progression/Economy", minRole: "GM_OPERATOR", description: "Award experience points", keywords: ["xp", "exp"] },
  { name: "AddItem", domain: "Progression/Economy", minRole: "GM_ARCHITECT", description: "Grant catalog item to inventory", keywords: ["loot", "gear", "give"] },
  { name: "AddCurrency", domain: "Progression/Economy", minRole: "GM_ARCHITECT", description: "Adjust gold or node currency balance", keywords: ["gold", "coin", "glint"] },
  { name: "SetNodeStage", domain: "World State", minRole: "GM_ARCHITECT", description: "Set Node of Power stage (0-6)", keywords: ["metropolis", "village", "tier"] },
  { name: "TriggerSiege", domain: "World State", minRole: "GM_ARCHITECT", description: "Initiate node or castle siege window", keywords: ["war", "battle"] },
  { name: "SetCaravanState", domain: "World State", minRole: "GM_OPERATOR", description: "Transition active caravan state machine", keywords: ["convoy", "trade"] },
  { name: "SetWeather", domain: "World State", minRole: "GM_OPERATOR", description: "Override regional weather profile", keywords: ["storm", "rain", "climate"] },
  { name: "Ban", domain: "Moderation", minRole: "GM_ARCHITECT", description: "Suspend account with audit reason", keywords: ["suspend", "block"] },
  { name: "Mute", domain: "Moderation", minRole: "GM_OPERATOR", description: "Silence chat channels for account", keywords: ["silence", "chat"] },
  { name: "Freeze", domain: "Moderation", minRole: "GM_OPERATOR", description: "Root player movement for investigation", keywords: ["hold", "lock"] },
  { name: "InspectNavTile", domain: "Diagnostics", minRole: "GM_OBSERVER", description: "Inspect Detour navmesh polygon tile", keywords: ["navmesh", "path"] },
  { name: "RaycastLos", domain: "Diagnostics", minRole: "GM_OBSERVER", description: "Probe BVH line-of-sight occlusion ray", keywords: ["bvh", "los", "ray"] },
  { name: "DumpEntityState", domain: "Diagnostics", minRole: "GM_OBSERVER", description: "Snapshot authoritative entity replication state", keywords: ["inspect", "debug"] },
]);

function requireNonEmpty(value: string, fieldName: string): string {
  const trimmed = value.trim();
  if (trimmed.length === 0) throw new RangeError(`${fieldName} is required`);
  return trimmed;
}

function requireFinite(value: number, fieldName: string): number {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    throw new RangeError(`${fieldName} must be a finite number`);
  }
  return value;
}

export function buildTeleportToPlayerCommand(character: string, targetPlayer: string): TeleportToPlayerCommand {
  return {
    command: "teleport_player",
    character: requireNonEmpty(character, "character"),
    target_player: requireNonEmpty(targetPlayer, "target_player"),
  };
}

export function buildTeleportToPlaceCommand(character: string, place: string): TeleportToPlaceCommand {
  return {
    command: "teleport_player",
    character: requireNonEmpty(character, "character"),
    place: requireNonEmpty(place, "place"),
  };
}

export function buildTeleportToCoordsCommand(character: string, xCm: number, yCm: number, zCm = 0): TeleportToCoordsCommand {
  return {
    command: "teleport_player",
    character: requireNonEmpty(character, "character"),
    x_cm: requireFinite(xCm, "x_cm"),
    y_cm: requireFinite(yCm, "y_cm"),
    z_cm: requireFinite(zCm, "z_cm"),
  };
}

export function buildTeleportPlayerCommand(input: TeleportPlayerInput): TeleportPlayerCommandPayload {
  const character = requireNonEmpty(input.character, "character");
  if (input.target_player && input.target_player.trim().length > 0) {
    return buildTeleportToPlayerCommand(character, input.target_player);
  }
  if (input.place && input.place.trim().length > 0) {
    return buildTeleportToPlaceCommand(character, input.place);
  }
  return buildTeleportToCoordsCommand(
    character,
    requireFinite(input.x_cm ?? Number.NaN, "x_cm"),
    requireFinite(input.y_cm ?? Number.NaN, "y_cm"),
    input.z_cm ?? 0,
  );
}

export function buildGiveMoneyCommand(character: string, gold: number, silver = 0, copper = 0): GiveMoneyCommand {
  const g = Math.max(0, Math.trunc(requireFinite(gold, "gold")));
  const s = Math.max(0, Math.trunc(requireFinite(silver, "silver")));
  const c = Math.max(0, Math.trunc(requireFinite(copper, "copper")));
  if (g + s + c <= 0) throw new RangeError("Total currency granted must be greater than zero");
  return { command: "give_money", character: requireNonEmpty(character, "character"), gold: g, silver: s, copper: c };
}

export function buildGrantItemCommand(character: string, item: string, count = 1, rarity: ItemRarity = "Common"): GrantItemCommand {
  const cleanCount = Math.max(1, Math.min(100, Math.trunc(requireFinite(count, "count"))));
  return {
    command: "grant_item",
    character: requireNonEmpty(character, "character"),
    item: requireNonEmpty(item, "item"),
    count: cleanCount,
    rarity,
  };
}

export function buildSpawnNpcCommand(input: SpawnNpcCommandInput): SpawnNpcCommand {
  const npc = requireNonEmpty(input.npc, "npc");
  const place = (input.place ?? "").trim();
  const xCm = input.x_cm !== undefined ? requireFinite(input.x_cm, "x_cm") : 0;
  const yCm = input.y_cm !== undefined ? requireFinite(input.y_cm, "y_cm") : 0;
  const zCm = input.z_cm !== undefined ? requireFinite(input.z_cm, "z_cm") : 0;
  if (place.length === 0 && input.x_cm === undefined && input.y_cm === undefined) {
    throw new RangeError("spawn_npc requires either a place or (x_cm, y_cm) coordinates");
  }
  const cleanLevel = Math.max(1, Math.min(50, Math.trunc(requireFinite(input.level ?? 1, "level"))));
  return { command: "spawn_npc", npc, place, x_cm: xCm, y_cm: yCm, z_cm: zCm, level: cleanLevel };
}

export function buildSetNodeLevelCommand(node: string, level: number): SetNodeLevelCommand {
  const cleanNode = requireNonEmpty(node, "node");
  const cleanLevel = Math.max(0, Math.min(6, Math.trunc(requireFinite(level, "level"))));
  return { command: "set_node_level", node: cleanNode, level: cleanLevel, plug_tier: nodeLevelToPlugTier(cleanLevel) };
}

export function buildPaginatedQueryUrl(basePath: string, params: PaginatedQueryParams = {}): string {
  const parts: string[] = [];
  if (params.q !== undefined && params.q.trim().length > 0) {
    parts.push(`q=${encodeURIComponent(params.q.trim())}`);
  }
  if (params.limit !== undefined && Number.isFinite(params.limit)) {
    parts.push(`limit=${Math.max(1, Math.min(500, Math.trunc(params.limit)))}`);
  }
  if (params.offset !== undefined && Number.isFinite(params.offset)) {
    parts.push(`offset=${Math.max(0, Math.trunc(params.offset))}`);
  }
  if (params.extra) {
    for (const [k, v] of Object.entries(params.extra)) {
      parts.push(`${encodeURIComponent(k)}=${encodeURIComponent(String(v))}`);
    }
  }
  return parts.length > 0 ? `${basePath}?${parts.join("&")}` : basePath;
}

export function formatLogEntryToString(entry: unknown): string {
  if (typeof entry === "string") return entry;
  if (entry === null || entry === undefined) return "";
  if (typeof entry !== "object") return String(entry);
  const obj = entry as Record<string, unknown>;
  if (typeof obj.raw === "string" && obj.raw.trim().length > 0) {
    return obj.raw.trimEnd();
  }
  const ts =
    typeof obj.timestamp === "string" && obj.timestamp.trim().length > 0
      ? obj.timestamp.trim()
      : typeof obj.time === "string" && obj.time.trim().length > 0
        ? obj.time.trim()
        : "";
  const container = typeof obj.container === "string" ? obj.container.trim() : "";
  const stream = typeof obj.stream === "string" ? obj.stream.trim() : "";
  const level = typeof obj.level === "string" ? obj.level.trim().toUpperCase() : "";
  const message =
    typeof obj.message === "string"
      ? obj.message
      : typeof obj.msg === "string"
        ? obj.msg
        : "";
  const prefix = [ts, container, stream, level].filter((p) => p.length > 0).join(" | ");
  if (message.length > 0) {
    return prefix.length > 0 ? `[${prefix}] ${message}` : message;
  }
  try {
    return JSON.stringify(obj);
  } catch {
    return "";
  }
}

export function normalizePaginatedEnvelope<T>(
  raw: unknown,
  fallbackKeys: readonly string[] = ["players", "places", "npcs", "containers", "lines"],
  defaultLimit = 25,
  defaultOffset = 0,
): PaginatedListEnvelope<T> {
  const obj = (raw && typeof raw === "object" ? raw : {}) as Record<string, unknown>;
  let items: readonly T[] = [];
  let matchedKey = false;
  for (const key of fallbackKeys) {
    if (Array.isArray(obj[key])) {
      items = obj[key] as readonly T[];
      matchedKey = true;
      break;
    }
  }
  if (!matchedKey && Array.isArray(obj.items)) {
    items = obj.items as readonly T[];
  }
  if (
    fallbackKeys[0] === "lines" &&
    items.length > 0 &&
    typeof items[0] === "object" &&
    items[0] !== null
  ) {
    items = items.map((entry) => formatLogEntryToString(entry)) as unknown as readonly T[];
  }
  const limit = typeof obj.limit === "number" && Number.isFinite(obj.limit) ? obj.limit : defaultLimit;
  const offset = typeof obj.offset === "number" && Number.isFinite(obj.offset) ? obj.offset : defaultOffset;
  const total =
    typeof obj.total === "number" && Number.isFinite(obj.total)
      ? obj.total
      : typeof obj.count === "number" && Number.isFinite(obj.count)
        ? obj.count
        : offset + items.length;
  const hasMore = typeof obj.has_more === "boolean" ? obj.has_more : offset + items.length < total;
  const nextCursor =
    typeof obj.next_cursor === "string" && obj.next_cursor.length > 0
      ? obj.next_cursor
      : hasMore
        ? String(offset + limit)
        : null;
  return { items, total, limit, offset, has_more: hasMore, next_cursor: nextCursor };
}

export function parseRfc9457Problem(status: number, raw: unknown): Rfc9457ProblemDetails {
  const obj = (raw && typeof raw === "object" ? raw : {}) as Record<string, unknown>;
  const title = typeof obj.title === "string" && obj.title.trim().length > 0 ? obj.title.trim() : `HTTP ${status}`;
  const detail =
    typeof obj.detail === "string" && obj.detail.trim().length > 0
      ? obj.detail.trim()
      : typeof obj.error === "string" && obj.error.trim().length > 0
        ? obj.error.trim()
        : typeof obj.message === "string" && obj.message.trim().length > 0
          ? obj.message.trim()
          : `Request failed with status ${status}`;
  return {
    type: typeof obj.type === "string" ? obj.type : "about:blank",
    title,
    status: typeof obj.status === "number" ? obj.status : status,
    detail,
    instance: typeof obj.instance === "string" ? obj.instance : undefined,
    code: typeof obj.code === "string" ? obj.code : undefined,
  };
}

export const TRANSIENT_HTTP_STATUSES: readonly number[] = Object.freeze([502, 503, 504]);
export const MAX_TRANSIENT_RETRIES = 2;

export interface TransientRetryOptions {
  readonly maxRetries?: number;
  readonly baseDelayMs?: number;
  readonly maxDelayMs?: number;
  readonly fetchImpl?: typeof fetch;
  readonly correlationId?: string;
}

export function computeBackoffWithJitterMs(
  attempt: number,
  baseDelayMs = 25,
  jitterSeed = 0.5,
  maxDelayMs = RETRY_MAX_DELAY_MS,
): number {
  const cleanAttempt = Math.max(0, Math.min(20, Math.trunc(attempt)));
  const expMs = Math.min(Math.max(1, maxDelayMs), Math.max(1, baseDelayMs) * Math.pow(2, cleanAttempt));
  const clampedSeed = Math.max(0, Math.min(1, jitterSeed));
  return Math.round(expMs * (0.5 + 0.5 * clampedSeed));
}

function waitForRetryOrReset(waitMs: number): Promise<boolean> {
  const startEpoch = getRetryResetEpoch();
  return new Promise<boolean>((resolve) => {
    let settled = false;
    const unsub = subscribeRetryReset(() => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      unsub();
      resolve(true);
    });
    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      unsub();
      resolve(getRetryResetEpoch() !== startEpoch);
    }, waitMs);
    registerPendingRetryTimer(timer);
  });
}

interface CachedEtagEntry {
  readonly etag: string;
  readonly lastModified?: string;
  readonly bodyText: string;
  readonly xPagination?: string;
}

const etagResponseCache = new Map<string, CachedEtagEntry>();
const MAX_ETAG_CACHE_ENTRIES = 256;

export function clearEtagResponseCache(): void {
  etagResponseCache.clear();
}

subscribeRetryReset(() => clearEtagResponseCache());

async function resolveEtagCachedResponse(url: string, response: Response, cached?: CachedEtagEntry): Promise<Response> {
  if (response.status === 304 && cached) {
    return new Response(cached.bodyText, {
      status: 200,
      headers: { "Content-Type": "application/json; charset=utf-8", ETag: cached.etag, ...(cached.xPagination ? { "X-Pagination": cached.xPagination } : {}), "X-Cache": "HIT-304" },
    });
  }
  if (response.status === 200 && typeof response.clone === "function") {
    const etag = response.headers?.get?.("ETag") ?? response.headers?.get?.("etag") ?? "";
    if (etag) {
      const bodyText = await response.clone().text().catch(() => "");
      if (bodyText) {
        if (etagResponseCache.size >= MAX_ETAG_CACHE_ENTRIES) {
          const oldest = etagResponseCache.keys().next().value;
          if (oldest) etagResponseCache.delete(oldest);
        }
        etagResponseCache.set(url, { etag, lastModified: response.headers?.get?.("Last-Modified") ?? undefined, bodyText, xPagination: response.headers?.get?.("X-Pagination") ?? undefined });
      }
    }
  }
  return response;
}

let fallbackSeqCounter = 0;

function nextRandomSeq12Bit(): number {
  if (typeof globalThis.crypto?.getRandomValues === "function") {
    return globalThis.crypto.getRandomValues(new Uint16Array(1))[0] & 0x0fff;
  }
  fallbackSeqCounter = (fallbackSeqCounter + 1) & 0x0fff;
  return fallbackSeqCounter;
}

export async function fetchWithTransientRetry(
  url: string,
  init: RequestInit = {},
  options: TransientRetryOptions = {},
): Promise<Response> {
  const maxRetries = options.maxRetries ?? MAX_TRANSIENT_RETRIES;
  const baseDelayMs = options.baseDelayMs ?? 15;
  const maxDelayMs = options.maxDelayMs ?? RETRY_MAX_DELAY_MS;
  const fetchFn = options.fetchImpl ?? fetch;
  const correlationId = options.correlationId ?? formatDeterministicUuidV7(Date.now(), nextRandomSeq12Bit());
  const method = (init.method ?? "GET").toUpperCase();
  if (method !== "GET" && method !== "HEAD") clearEtagResponseCache();
  const cached = method === "GET" ? etagResponseCache.get(url) : undefined;
  let lastError: unknown = null;
  for (let attempt = 0; attempt <= maxRetries; attempt += 1) {
    const baseHeaders: Record<string, string> = buildAdminAuthHeaders({
      Accept: "application/json",
      "X-Correlation-ID": correlationId,
      ...(init.body !== undefined ? { "Content-Type": "application/json; charset=utf-8" } : {}),
      ...(cached ? { "If-None-Match": cached.etag, ...(cached.lastModified ? { "If-Modified-Since": cached.lastModified } : {}) } : {}),
      ...((init.headers as Record<string, string> | undefined) ?? {}),
    });
    try {
      const response = await fetchFn(url, { ...init, headers: baseHeaders });
      if ((response.status === 401 || response.status === 403) && url.includes("/api/v1/") && !url.includes("/api/v1/admin/auth/login")) {
        handleUnauthorizedStatus(response.status);
        return response;
      }
      if (!TRANSIENT_HTTP_STATUSES.includes(response.status) || attempt === maxRetries) {
        return method === "GET" ? await resolveEtagCachedResponse(url, response, cached) : response;
      }
    } catch (err) {
      lastError = err;
      if (attempt === maxRetries) throw err;
    }
    const waitMs = computeBackoffWithJitterMs(attempt, baseDelayMs, (attempt + 1) * 0.3, maxDelayMs);
    await waitForRetryOrReset(waitMs);
  }
  throw lastError ?? new Error(`Transient retry exhausted for ${url}`);
}

export async function sendGmCommand(
  payload: GmActionCommandPayload,
  fetchImpl: typeof fetch = fetch,
): Promise<{
  readonly ok: boolean;
  readonly message: string;
  readonly correlationId: string;
  readonly idempotencyKey: string;
  readonly problem?: Rfc9457ProblemDetails;
}> {
  const seq = nextRandomSeq12Bit();
  const idemKey = formatDeterministicUuidV7(Date.now(), seq);
  const correlationId = formatDeterministicUuidV7(Date.now(), seq + 1);
  const res = await fetchWithTransientRetry(
    "/api/v1/gm/commands",
    {
      method: "POST",
      headers: {
        Accept: "application/json",
        "Content-Type": "application/json; charset=utf-8",
        "X-Correlation-ID": correlationId,
        "Idempotency-Key": idemKey,
        "X-Idempotency-Key": idemKey,
      },
      body: JSON.stringify(payload),
    },
    { fetchImpl, correlationId, baseDelayMs: 5 },
  );
  const data = (await res.json().catch(() => ({}))) as Record<string, unknown>;
  if (!res.ok) {
    const problem = parseRfc9457Problem(res.status, data);
    return { ok: false, message: `${problem.title}: ${problem.detail}`, correlationId, idempotencyKey: idemKey, problem };
  }
  const msg =
    typeof data.message === "string"
      ? data.message
      : typeof data.status === "string"
        ? data.status
        : `${payload.command} executed`;
  return { ok: true, message: msg, correlationId, idempotencyKey: idemKey };
}

export function validateGmCommandRbac(role: GmRole, command: GmCommandName): boolean {
  const def = GM_COMMAND_CATALOG.find((item) => item.name === command);
  return def ? ROLE_RANK[role] >= ROLE_RANK[def.minRole] : false;
}

export function formatDeterministicUuidV7(timestampMs: number, sequence: number, nodeSeed = 0x53414e44): string {
  if (!Number.isFinite(timestampMs) || timestampMs < 0 || !Number.isFinite(sequence) || sequence < 0) {
    throw new RangeError("UUIDv7 timestampMs and sequence must be non-negative finite numbers");
  }
  const tsHex = Math.trunc(timestampMs).toString(16).padStart(12, "0").slice(-12);
  const randA = (Math.trunc(sequence) & 0x0fff).toString(16).padStart(3, "0");
  const variantNibble = (0x8 | ((Math.trunc(sequence) >>> 12) & 0x3)).toString(16);
  const randBHigh = ((Math.trunc(sequence) >>> 14) & 0x0fff).toString(16).padStart(3, "0");
  const nodeHex = (Math.trunc(nodeSeed) >>> 0).toString(16).padStart(8, "0");
  const tailHex = (nodeHex + ((Math.trunc(sequence) * 2654435761) >>> 0).toString(16).padStart(8, "0")).slice(0, 12);
  return `${tsHex.slice(0, 8)}-${tsHex.slice(8, 12)}-7${randA}-${variantNibble}${randBHigh}-${tailHex}`;
}

export function computeFuzzyScore(query: string, candidate: string): number {
  const cleanQuery = query.trim().toLowerCase();
  const cleanTarget = candidate.toLowerCase();
  if (cleanQuery.length === 0) return 1;
  if (cleanTarget === cleanQuery) return 100;
  if (cleanTarget.startsWith(cleanQuery)) return 85;
  if (cleanTarget.includes(cleanQuery)) return 65;
  let qIdx = 0;
  for (let i = 0; i < cleanTarget.length && qIdx < cleanQuery.length; i += 1) {
    if (cleanTarget[i] === cleanQuery[qIdx]) qIdx += 1;
  }
  return qIdx === cleanQuery.length ? 40 : 0;
}

export function filterGmCommandPalette(
  query: string,
  catalogItems: readonly GmCatalogItem[] = [],
  domainFilter?: GmCommandDomain,
): readonly GmPaletteOption[] {
  const results: GmPaletteOption[] = [];
  for (const cmd of GM_COMMAND_CATALOG) {
    if (domainFilter && cmd.domain !== domainFilter) continue;
    const score = computeFuzzyScore(query, `${cmd.name} ${cmd.domain} ${cmd.description} ${cmd.keywords.join(" ")}`);
    if (score > 0) {
      results.push({
        id: `cmd:${cmd.name}`,
        command: cmd.name,
        domain: cmd.domain,
        minRole: cmd.minRole,
        label: `${cmd.name} - ${cmd.description}`,
        score,
        catalogItemId: null,
      });
    }
  }
  for (const item of catalogItems) {
    const def = GM_COMMAND_CATALOG.find((c) => c.name === item.command);
    if (!def || (domainFilter && def.domain !== domainFilter)) continue;
    const score = computeFuzzyScore(query, `${item.label} ${item.id} ${item.category} ${item.command}`);
    if (score > 0) {
      results.push({
        id: `cat:${item.id}`,
        command: item.command,
        domain: def.domain,
        minRole: def.minRole,
        label: `${item.command}: ${item.label} (${item.id})`,
        score: score + 5,
        catalogItemId: item.id,
      });
    }
  }
  return results.sort((a, b) => b.score - a.score || a.label.localeCompare(b.label));
}

export function buildGmCommandRequest(input: GmCommandBuildInput): GmCommandEnvelope {
  const def = GM_COMMAND_CATALOG.find((c) => c.name === input.command);
  if (!def) throw new RangeError(`Unknown GM command: ${input.command}`);
  if (!validateGmCommandRbac(input.role, input.command)) {
    throw new Error(`Role ${input.role} is not authorized for ${input.command} (requires ${def.minRole})`);
  }
  const idempotencyKey = formatDeterministicUuidV7(input.timestampMs, input.sequence);
  return {
    endpoint: "/api/v1/gm/command",
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "Idempotency-Key": idempotencyKey,
      "X-Idempotency-Key": idempotencyKey,
    },
    body: {
      command_type: input.command,
      domain: def.domain,
      actor_role: input.role,
      target_guid: input.targetGuid ?? null,
      payload: input.payload ?? {},
    },
  };
}

export function GmCommandPalette(props: GmCommandPaletteProps): React.ReactElement | null {
  if (!props.isOpen) return null;
  const options = filterGmCommandPalette(props.query, props.catalogItems ?? []);
  const activeIdx = Math.min(Math.max(0, props.activeIndex ?? 0), Math.max(0, options.length - 1));
  return (
    <div
      role="dialog"
      aria-modal="true"
      aria-label="GM Studio Command Palette"
      data-testid="gm-command-palette-dialog"
      style={{ position: "fixed", inset: 0, zIndex: 50, display: "flex", alignItems: "flex-start", justifyContent: "center", paddingTop: "64px", backgroundColor: "rgba(9, 13, 22, 0.78)" }}
    >
      <div style={{ width: "520px", maxWidth: "94vw", borderRadius: "8px", border: "1px solid rgba(140, 108, 66, 0.45)", backgroundColor: "rgba(13, 17, 23, 0.96)", color: "rgba(240, 246, 252, 1)", padding: "12px", fontVariantNumeric: "tabular-nums" }}>
        <input
          type="text"
          role="combobox"
          aria-expanded="true"
          aria-controls="gm-command-palette-listbox"
          aria-label="Search GM commands and world catalog"
          data-testid="gm-command-palette-input"
          value={props.query}
          onChange={(e) => props.onQueryChange?.(e.currentTarget.value)}
          placeholder="Type a GM command or catalog entity..."
          style={{ width: "100%", minHeight: "34px", padding: "6px 10px", borderRadius: "6px", border: "1px solid rgba(140, 108, 66, 0.45)", backgroundColor: "rgba(22, 27, 34, 0.96)", color: "rgba(240, 246, 252, 1)", fontSize: "13px" }}
        />
        <ul
          id="gm-command-palette-listbox"
          role="listbox"
          aria-label="Filtered GM commands"
          data-testid="gm-command-palette-results"
          style={{ maxHeight: "260px", overflowY: "auto", margin: "8px 0 0 0", padding: 0, listStyle: "none" }}
        >
          {options.map((opt, idx) => (
            <li
              key={opt.id}
              role="option"
              aria-selected={idx === activeIdx}
              aria-disabled={!validateGmCommandRbac(props.role, opt.command)}
              data-testid={`gm-command-option-${opt.command}`}
              onClick={() => props.onExecuteCommand?.(opt.command, opt.catalogItemId)}
              style={{ padding: "6px 8px", borderRadius: "4px", cursor: "pointer", backgroundColor: idx === activeIdx ? "rgba(31, 111, 235, 0.85)" : "transparent", color: "rgba(240, 246, 252, 1)", fontSize: "12px", display: "flex", justifyContent: "space-between" }}
            >
              <span>{opt.label}</span>
              <span style={{ color: "rgba(201, 209, 217, 1)", fontSize: "11px" }}>{opt.domain} [{opt.minRole}]</span>
            </li>
          ))}
        </ul>
      </div>
    </div>
  );
}

export default GmCommandPalette;
