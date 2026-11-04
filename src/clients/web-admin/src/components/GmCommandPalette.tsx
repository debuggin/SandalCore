import React from "react";

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

export interface GmCommandDefinition {
  readonly name: GmCommandName;
  readonly domain: GmCommandDomain;
  readonly minRole: GmRole;
  readonly description: string;
  readonly keywords: readonly string[];
}

export interface GmCatalogItem {
  readonly id: string;
  readonly label: string;
  readonly category: "npc" | "item" | "node" | "weather";
  readonly command: GmCommandName;
}

export interface GmPaletteOption {
  readonly id: string;
  readonly command: GmCommandName;
  readonly domain: GmCommandDomain;
  readonly minRole: GmRole;
  readonly label: string;
  readonly score: number;
  readonly catalogItemId: string | null;
}

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

export function validateGmCommandRbac(role: GmRole, command: GmCommandName): boolean {
  const def = GM_COMMAND_CATALOG.find((item) => item.name === command);
  if (!def) {
    return false;
  }
  return ROLE_RANK[role] >= ROLE_RANK[def.minRole];
}

// Formats an RFC 9562 UUIDv7 string deterministically from caller-supplied epoch ms and monotonic counter.
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
  if (cleanQuery.length === 0) {
    return 1;
  }
  if (cleanTarget === cleanQuery) {
    return 100;
  }
  if (cleanTarget.startsWith(cleanQuery)) {
    return 85;
  }
  if (cleanTarget.includes(cleanQuery)) {
    return 65;
  }
  let qIdx = 0;
  for (let i = 0; i < cleanTarget.length && qIdx < cleanQuery.length; i += 1) {
    if (cleanTarget[i] === cleanQuery[qIdx]) {
      qIdx += 1;
    }
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
    if (domainFilter && cmd.domain !== domainFilter) {
      continue;
    }
    const searchable = `${cmd.name} ${cmd.domain} ${cmd.description} ${cmd.keywords.join(" ")}`;
    const score = computeFuzzyScore(query, searchable);
    if (score > 0) {
      results.push({ id: `cmd:${cmd.name}`, command: cmd.name, domain: cmd.domain, minRole: cmd.minRole, label: `${cmd.name} — ${cmd.description}`, score, catalogItemId: null });
    }
  }
  for (const item of catalogItems) {
    const def = GM_COMMAND_CATALOG.find((c) => c.name === item.command);
    if (!def || (domainFilter && def.domain !== domainFilter)) {
      continue;
    }
    const score = computeFuzzyScore(query, `${item.label} ${item.id} ${item.category} ${item.command}`);
    if (score > 0) {
      results.push({ id: `cat:${item.id}`, command: item.command, domain: def.domain, minRole: def.minRole, label: `${item.command}: ${item.label} (${item.id})`, score: score + 5, catalogItemId: item.id });
    }
  }
  return results.sort((a, b) => b.score - a.score || a.label.localeCompare(b.label));
}

export function buildGmCommandRequest(input: GmCommandBuildInput): GmCommandEnvelope {
  const def = GM_COMMAND_CATALOG.find((c) => c.name === input.command);
  if (!def) {
    throw new RangeError(`Unknown GM command: ${input.command}`);
  }
  if (!validateGmCommandRbac(input.role, input.command)) {
    throw new Error(`Role ${input.role} is not authorized for ${input.command} (requires ${def.minRole})`);
  }
  const idempotencyKey = formatDeterministicUuidV7(input.timestampMs, input.sequence);
  return {
    endpoint: "/api/v1/gm/command",
    method: "POST",
    headers: { "Content-Type": "application/json", "Idempotency-Key": idempotencyKey, "X-Idempotency-Key": idempotencyKey },
    body: { command_type: input.command, domain: def.domain, actor_role: input.role, target_guid: input.targetGuid ?? null, payload: input.payload ?? {} },
  };
}

export function GmCommandPalette(props: GmCommandPaletteProps): React.ReactElement | null {
  if (!props.isOpen) {
    return null;
  }
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
      <div style={{ width: "520px", maxWidth: "94vw", borderRadius: "8px", border: "1px solid #30363D", backgroundColor: "#0D1117", color: "#F0F6FC", padding: "12px", fontVariantNumeric: "tabular-nums" }}>
        <input
          type="text"
          role="combobox"
          aria-expanded="true"
          aria-controls="gm-command-palette-listbox"
          aria-label="Search GM commands and world catalog"
          data-testid="gm-command-palette-input"
          value={props.query}
          onChange={(e) => props.onQueryChange?.(e.currentTarget.value)}
          placeholder="Type a GM command or catalog entity (Cmd+K)..."
          style={{ width: "100%", minHeight: "34px", padding: "6px 10px", borderRadius: "6px", border: "1px solid #30363D", backgroundColor: "#161B22", color: "#F0F6FC", fontSize: "13px" }}
        />
        <ul id="gm-command-palette-listbox" role="listbox" aria-label="Filtered GM commands" data-testid="gm-command-palette-results" style={{ maxHeight: "260px", overflowY: "auto", margin: "8px 0 0 0", padding: 0, listStyle: "none" }}>
          {options.map((opt, idx) => (
            <li
              key={opt.id}
              role="option"
              aria-selected={idx === activeIdx}
              aria-disabled={!validateGmCommandRbac(props.role, opt.command)}
              data-testid={`gm-command-option-${opt.command}`}
              onClick={() => props.onExecuteCommand?.(opt.command, opt.catalogItemId)}
              style={{ padding: "6px 8px", borderRadius: "4px", cursor: "pointer", backgroundColor: idx === activeIdx ? "#1F6FEB" : "transparent", color: "#F0F6FC", fontSize: "12px", display: "flex", justifyContent: "space-between" }}
            >
              <span>{opt.label}</span>
              <span style={{ color: "#C9D1D9", fontSize: "11px" }}>{opt.domain} [{opt.minRole}]</span>
            </li>
          ))}
        </ul>
      </div>
    </div>
  );
}

export default GmCommandPalette;
