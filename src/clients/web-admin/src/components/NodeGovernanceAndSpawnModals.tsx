// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import {
  buildWorldStatePollUrl,
  getCityNodeCenterCm,
  inferResourceNodeType,
  NODE_STAGE_NAMES,
  type NodeElectionAction,
  resolveWorkerForCoordinateOrZone,
  useWorldStreamStore,
  type VerraCityNodeMeta,
  type WorldEntity,
  type WorldStreamState,
} from "../state/worldStreamStore";
import {
  buildSetNodeLevelCommand,
  buildSpawnNpcCommand,
  fetchWithTransientRetry,
  type PaginatedListEnvelope,
  sendGmCommand,
} from "./GmCommandPalette";

export const SANDAL_LAUNCHER_TOKENS = Object.freeze({
  VerraObsidian100: "#0B0E17",
  AoCFiligreeDeckBrush:
    "linear-gradient(135deg, rgba(11, 14, 23, 0.78) 0%, rgba(16, 23, 36, 0.84) 100%)",
  AoCCardBorderBrush: "rgba(140, 108, 66, 0.47)",
  VerraBorder400: "rgba(140, 108, 66, 0.67)",
  AoCInputFillBrush: "rgba(18, 25, 38, 0.92)",
  AoCSecondaryBtnBrush: "rgba(22, 30, 46, 0.92)",
  VerraRelicGold500: "#E8B25A",
  VerraChampagneGold400: "#F6CC7E",
  VerraSanctumEmerald500: "#20D8CE",
  VerraCautionAmber500: "#F5C469",
  VerraRuinCrimson500: "#E05258",
  VerraAlabaster100: "#F4F1EA",
  VerraMutedAsh400: "#B8C4D4",
});

export interface OnlinePlayerRecord {
  readonly character_id: string;
  readonly name: string;
  readonly x_cm: number;
  readonly y_cm: number;
  readonly z_cm: number;
  readonly online: boolean;
  readonly zone?: string;
}

export interface CatalogItemEntry {
  readonly guid: string;
  readonly name: string;
  readonly display_name: string;
  readonly equip_slot: string;
  readonly level: number;
}

export interface CatalogNpcEntry {
  readonly guid: string;
  readonly name: string;
  readonly display_name?: string;
  readonly category?: string;
  readonly default_level?: number;
  readonly subkind?: string;
  readonly min_level?: number;
  readonly max_level?: number;
}

export interface CatalogPlaceEntry {
  readonly guid: string;
  readonly name: string;
  readonly display_name: string;
  readonly x_cm: number;
  readonly y_cm: number;
  readonly z_cm: number;
}

export interface CatalogPickerOption {
  readonly value: string;
  readonly label: string;
  readonly detail?: string;
}

export interface CatalogComboboxPickerProps {
  readonly label: string;
  readonly testIdPrefix: string;
  readonly placeholder: string;
  readonly value: string;
  readonly text: string;
  readonly options: readonly CatalogPickerOption[];
  readonly presets?: readonly CatalogPickerOption[];
  readonly onTextChange: (nextText: string) => void;
  readonly onSelectOption: (opt: CatalogPickerOption) => void;
}

export const DEFAULT_ONLINE_PLAYERS: readonly OnlinePlayerRecord[] = Object.freeze([]);

export const DEFAULT_CATALOG_ITEMS: readonly CatalogItemEntry[] = Object.freeze([
  { guid: "6064629100001", name: "Item_Weapon_2H_Sword_Radiant", display_name: "Radiant Greatsword", equip_slot: "MainHand", level: 25 },
  { guid: "6064629100002", name: "Item_Consumable_HealthPotion", display_name: "Major Health Elixir", equip_slot: "Consumable", level: 10 },
  { guid: "6064629100003", name: "Item_Armor_Chest_SteelPlate", display_name: "Riverlands Steel Cuirass", equip_slot: "Chest", level: 20 },
]);

export const DEFAULT_CATALOG_NPCS: readonly CatalogNpcEntry[] = Object.freeze([
  { guid: "6064628876645793792", name: "Aela", category: "npc", default_level: 15, subkind: "ambient" },
  { guid: "6064628931065511936", name: "Riverlands Wolf", category: "monster", default_level: 12, subkind: "hostile" },
  { guid: "6064628955249868800", name: "Citizen", category: "npc", default_level: 20, subkind: "ambient" },
]);

export const DEFAULT_CATALOG_PLACES: readonly CatalogPlaceEntry[] = Object.freeze([
  { guid: "108646108518000", name: "Verra_RVR_Lionhold", display_name: "Lionhold", x_cm: -451200.0, y_cm: 289400.0, z_cm: 11800.0 },
  { guid: "108646108518001", name: "Verra_RVR_SamiasHope", display_name: "Samia's Hope", x_cm: -582100.0, y_cm: 492300.0, z_cm: 12100.0 },
  { guid: "108646108518008", name: "Verra_RVR_Winstead", display_name: "Winstead", x_cm: -644519.11, y_cm: 376773.24, z_cm: 12012.23 },
  { guid: "108646608033403", name: "Verra_RVR_NewAela", display_name: "New Aela", x_cm: -1134183.54, y_cm: 853387.28, z_cm: 14800.0 },
  { guid: "108646121231993", name: "Verra_RVR_Joeva", display_name: "Joeva", x_cm: -1094396.04, y_cm: 521336.34, z_cm: 11836.45 },
  { guid: "108646610851452", name: "Verra_RVR_Halcyon", display_name: "Halcyon", x_cm: -891024.5, y_cm: 296919.66, z_cm: 11657.05 },
  { guid: "108646615701117", name: "Verra_RVR_Miraleth", display_name: "Miraleth", x_cm: -459889.63, y_cm: 599317.09, z_cm: 13100.0 },
]);

export const MOVE_SPEED_MULTIPLIERS: readonly number[] = Object.freeze([0.5, 1, 2, 5, 10]);

export async function dispatchAuxiliaryGmAction(
  body: Readonly<Record<string, unknown>>,
  fetchImpl: typeof fetch = typeof fetch === "function" ? fetch : (undefined as unknown as typeof fetch),
  storeSnap: WorldStreamState = useWorldStreamStore.getState(),
): Promise<{ readonly ok: boolean; readonly message: string }> {
  const action = String(body.action ?? "command");
  const entityGuid = String(body.entity_guid ?? body.npc_guid ?? body.guid ?? "").trim();
  if (typeof fetchImpl !== "function") return { ok: true, message: `${action} executed` };
  try {
    const res = await fetchImpl("/api/v1/gm/command", {
      method: "POST",
      headers: { "Content-Type": "application/json", Accept: "application/json" },
      body: JSON.stringify(body),
    });
    const data = (await res.json().catch(() => ({}))) as Record<string, unknown>;
    const msg = res.ok
      ? typeof data.message === "string" && data.message.length > 0
        ? data.message
        : `${action} executed`
      : String(data.detail || data.title || data.message || "HTTP " + res.status);
    if (res.ok && (action === "kill_npc" || action === "despawn_npc")) {
      if (entityGuid) {
        storeSnap.applyWorldDelta({ seq: storeSnap.lastSeq + 1, removed: [entityGuid] });
        if (storeSnap.selectedEntityGuid === entityGuid) storeSnap.selectEntity(null);
      }
      await refreshWorldEntitiesAfterNodeTierChange(storeSnap, fetchImpl);
    }
    return { ok: res.ok, message: msg };
  } catch (err) {
    return { ok: false, message: err instanceof Error ? err.message : `${action} failed` };
  }
}

export function CatalogComboboxPicker(props: CatalogComboboxPickerProps): React.ReactElement {
  const [open, setOpen] = React.useState(false);
  const [activeIndex, setActiveIndex] = React.useState(0);
  const listId = `${props.testIdPrefix}-listbox`;
  const q = props.text.trim().toLowerCase();
  const visibleOptions = q && !props.options.some((o) => o.label.toLowerCase() === q)
    ? props.options.filter((o) => o.label.toLowerCase().includes(q) || o.value.toLowerCase().includes(q))
    : props.options;
  const choose = (opt: CatalogPickerOption): void => { props.onSelectOption(opt); setOpen(false); };
  return (
    <div data-testid={`${props.testIdPrefix}-picker`} style={{ position: "relative", marginBottom: "8px" }}>
      <label style={{ display: "block", marginBottom: "4px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, fontSize: "11px" }}>{props.label}</label>
      <input
        type="search" role="combobox" aria-label={props.label} aria-expanded={open} aria-controls={listId} data-testid={`${props.testIdPrefix}-search`}
        value={props.text} placeholder={props.placeholder} onFocus={() => setOpen(true)} onBlur={() => setOpen(false)}
        onChange={(e) => { setOpen(true); setActiveIndex(0); props.onTextChange(e.currentTarget.value); }}
        onKeyDown={(e) => {
          if (e.key === "Escape") setOpen(false);
          else if (e.key === "ArrowDown") setActiveIndex((i) => Math.min(Math.max(0, visibleOptions.length - 1), i + 1));
          else if (e.key === "ArrowUp") setActiveIndex((i) => Math.max(0, i - 1));
          else if (e.key === "Enter" && open && visibleOptions[activeIndex]) { e.preventDefault(); choose(visibleOptions[activeIndex]); }
        }}
        style={{ ...INPUT_STYLE, width: "100%" }}
      />
      {open && visibleOptions.length > 0 ? (
        <ul id={listId} role="listbox" data-testid={`${props.testIdPrefix}-options`} style={{ position: "absolute", top: "58px", left: 0, right: 0, maxHeight: "200px", overflowY: "auto", margin: 0, padding: "4px", listStyle: "none", borderRadius: "6px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.VerraBorder400}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.VerraObsidian100, zIndex: 10020 }}>
          {visibleOptions.map((opt, idx) => (
            <li key={opt.value} role="option" aria-selected={idx === activeIndex} data-testid={`${props.testIdPrefix}-option-${opt.value}`} onMouseDown={(e) => { e.preventDefault(); choose(opt); }} style={{ padding: "6px 8px", borderRadius: "4px", cursor: "pointer", backgroundColor: idx === activeIndex ? "rgba(232, 178, 90, 0.22)" : "transparent", color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, display: "flex", justifyContent: "space-between", fontSize: "12px" }}>
              <span>{opt.label}</span>
              {opt.detail ? <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, fontSize: "11px" }}>{opt.detail}</span> : null}
            </li>
          ))}
        </ul>
      ) : null}
    </div>
  );
}

const CARD_STYLE: React.CSSProperties = {
  padding: "14px", borderRadius: "10px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
  background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush, backdropFilter: "blur(16px)",
  boxShadow: "0 12px 32px rgba(0, 0, 0, 0.55)", color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "12px",
};

const INPUT_STYLE: React.CSSProperties = {
  minHeight: "32px", padding: "5px 8px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
  backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "12px",
};

const CHIP_STYLE: React.CSSProperties = {
  minHeight: "32px", padding: "4px 10px", borderRadius: "999px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
  backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100,
  fontSize: "11px", fontWeight: 600, cursor: "pointer",
};

const BTN_STYLE: React.CSSProperties = {
  minHeight: "32px", padding: "6px 14px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.VerraBorder400}`,
  background: "linear-gradient(135deg, #E8B25A 0%, #B8842D 100%)", color: SANDAL_LAUNCHER_TOKENS.VerraObsidian100,
  fontSize: "12px", fontWeight: 700, cursor: "pointer",
};

export interface SpawnNpcCardProps {
  readonly npcEnv: PaginatedListEnvelope<CatalogNpcEntry>;
  readonly npcQuery: string;
  readonly places: readonly CatalogPlaceEntry[];
  readonly players: readonly OnlinePlayerRecord[];
  readonly onNpcQueryChange: (q: string) => void;
}

async function executeSpawnNpcSubmission(
  storeSnap: WorldStreamState,
  ctx: { readonly selectedNpc: string; readonly level: number; readonly place: string; readonly coords: { readonly x: number; readonly y: number; readonly z: number } },
  places: readonly CatalogPlaceEntry[],
  players: readonly OnlinePlayerRecord[],
): Promise<void> {
  try {
    const targetMode = storeSnap.spawnTargetMode;
    const pin = storeSnap.lastClickedPoint ?? storeSnap.pickedCoordinate;
    const playerObj = players.find((p) => p.name === storeSnap.selectedPlayer) ?? players[0];
    const placeObj = places.find((pl) => pl.name === ctx.place);
    const targetX = targetMode === "place" ? Number(placeObj?.x_cm ?? -644519) : targetMode === "player" && playerObj ? Number(playerObj.x_cm ?? 0) : pin ? Number(pin.xCm ?? 0) : ctx.coords.x;
    const targetY = targetMode === "place" ? Number(placeObj?.y_cm ?? 376773) : targetMode === "player" && playerObj ? Number(playerObj.y_cm ?? 0) : pin ? Number(pin.yCm ?? 0) : ctx.coords.y;
    const worker = resolveWorkerForCoordinateOrZone(targetX, targetY, undefined, storeSnap.workers, targetMode === "place" ? ctx.place : undefined);
    const cmd = targetMode === "place"
      ? buildSpawnNpcCommand({ npc: ctx.selectedNpc, place: ctx.place, level: ctx.level })
      : targetMode === "player" && playerObj
        ? buildSpawnNpcCommand({ npc: ctx.selectedNpc, x_cm: Number(playerObj.x_cm ?? 0), y_cm: Number(playerObj.y_cm ?? 0), z_cm: Number(playerObj.z_cm ?? 0), level: ctx.level })
        : buildSpawnNpcCommand({ npc: ctx.selectedNpc, x_cm: pin ? Number(pin.xCm ?? 0) : ctx.coords.x, y_cm: pin ? Number(pin.yCm ?? 0) : ctx.coords.y, z_cm: pin ? Number(pin.zCm ?? 0) : ctx.coords.z, level: ctx.level });
    const res = await sendGmCommand({ ...cmd, zone: worker.scope_key, worker_id: worker.worker_id } as never);
    const spawnZ = targetMode === "player" && playerObj ? Number(playerObj.z_cm ?? 0) : pin ? Number(pin.zCm ?? 0) : ctx.coords.z;
    const whereStr = targetMode === "place" ? (placeObj?.display_name ?? ctx.place) : `(${Math.round(targetX)}, ${Math.round(targetY)}, ${Math.round(spawnZ)})`;
    const prose = `Spawned Lv ${ctx.level} ${ctx.selectedNpc} at ${whereStr}`;
    storeSnap.setScopedReceipt("spawnNpc", { ok: res.ok, message: res.ok ? prose : `${prose}: ${res.message}`, correlationId: res.correlationId });
    await refreshWorldEntitiesAfterNodeTierChange(storeSnap);
  } catch (err) {
    storeSnap.setScopedReceipt("spawnNpc", { ok: false, message: (err as Error).message });
  }
}

export function SpawnNpcCard(props: SpawnNpcCardProps): React.ReactElement {
  const { npcEnv, npcQuery, places, players, onNpcQueryChange } = props;
  const storeSnap = useWorldStreamStore();
  const catalogNpcs = npcEnv.items;
  const rawSelectedNpc = storeSnap.selectedNpcTemplate;
  const selectedNpc = rawSelectedNpc ? rawSelectedNpc : catalogNpcs[0]?.name || DEFAULT_CATALOG_NPCS[0]?.name || "Aela";
  const [level, setLevel] = React.useState(15);
  const targetMode = storeSnap.spawnTargetMode;
  const [place, setPlace] = React.useState(DEFAULT_CATALOG_PLACES[2]?.name ?? "Verra_RVR_Winstead");
  const pin = storeSnap.lastClickedPoint ?? storeSnap.pickedCoordinate;
  const [coords, setCoords] = React.useState({ x: Math.round(Number(pin?.xCm ?? -644519)), y: Math.round(Number(pin?.yCm ?? 376773)), z: Math.round(Number(pin?.zCm ?? 12012)) });
  React.useEffect(() => {
    if (pin) setCoords({ x: Math.round(Number(pin.xCm ?? -644519)), y: Math.round(Number(pin.yCm ?? 376773)), z: Math.round(Number(pin.zCm ?? 12012)) });
  }, [pin?.xCm, pin?.yCm, pin?.zCm]);
  React.useEffect(() => {
    if (catalogNpcs.length > 0 && !rawSelectedNpc) storeSnap.setSelectedNpcTemplate(catalogNpcs[0].name);
  }, [catalogNpcs, rawSelectedNpc]);
  const npcOptions: readonly CatalogPickerOption[] = catalogNpcs.map((n) => {
    const lvlStr = typeof n.default_level === "number" ? `Lv ${n.default_level}` : typeof n.min_level === "number" && typeof n.max_level === "number" ? `Lv ${n.min_level}-${n.max_level}` : "Lv 15";
    return { value: n.name, label: `${n.display_name || n.name} (${lvlStr})`, detail: n.name };
  });
  return (
    <section aria-label="Spawn NPC from Catalog (New Instance)" data-testid="spawn-npc-card" style={CARD_STYLE}>
      <strong style={{ display: "block", marginBottom: "8px", color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 }}>Spawn NPC Definition (New Instance)</strong>
      <CatalogComboboxPicker
        label="NPC Definition to Spawn (Click an already-spawned NPC on the map to inspect/manage it)"
        testIdPrefix="catalog-npcs" placeholder="Search NPC definitions by name or GUID..." value={selectedNpc} text={npcQuery}
        options={npcOptions} onTextChange={onNpcQueryChange}
        onSelectOption={(opt) => { storeSnap.setSelectedNpcTemplate(opt.value); onNpcQueryChange(opt.label); }}
      />
      <div style={{ display: "flex", gap: "6px", marginBottom: "8px", flexWrap: "wrap", alignItems: "center" }}>
        {([{ id: "coords", label: "At Map Location (X, Y, Z)" }, { id: "player", label: "At Player" }, { id: "place", label: "At Named Place" }] as const).map((m) => (
          <button key={m.id} type="button" data-testid={`spawn-mode-${m.id}`} onClick={() => storeSnap.setSpawnTargetMode(m.id)} style={{ ...CHIP_STYLE, backgroundColor: targetMode === m.id ? SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: targetMode === m.id ? SANDAL_LAUNCHER_TOKENS.VerraObsidian100 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100 }}>{m.label}</button>
        ))}
        <label style={{ display: "inline-flex", alignItems: "center", gap: "4px" }}>
          Level
          <input type="number" min={1} max={50} aria-label="NPC Level" data-testid="spawn-npc-level" value={level} onChange={(e) => setLevel(Number(e.currentTarget.value))} style={{ ...INPUT_STYLE, width: "72px" }} />
        </label>
      </div>
      <div style={{ display: "flex", gap: "6px", flexWrap: "wrap", alignItems: "center" }}>
        {targetMode === "place" ? (
          <select aria-label="Spawn Place" data-testid="spawn-npc-place" value={place} onChange={(e) => setPlace(e.currentTarget.value)} style={INPUT_STYLE}>
            {places.map((pl) => (<option key={pl.name} value={pl.name}>{pl.display_name}</option>))}
          </select>
        ) : targetMode === "coords" ? (
          <div style={{ display: "inline-flex", gap: "4px" }}>
            <input type="number" aria-label="Spawn X cm" data-testid="spawn-npc-x" value={coords.x} onChange={(e) => setCoords({ ...coords, x: Number(e.currentTarget.value) })} style={INPUT_STYLE} />
            <input type="number" aria-label="Spawn Y cm" data-testid="spawn-npc-y" value={coords.y} onChange={(e) => setCoords({ ...coords, y: Number(e.currentTarget.value) })} style={INPUT_STYLE} />
            <input type="number" aria-label="Spawn Z cm" data-testid="spawn-npc-z" value={coords.z} onChange={(e) => setCoords({ ...coords, z: Number(e.currentTarget.value) })} style={INPUT_STYLE} />
          </div>
        ) : null}
        <button type="button" data-testid="spawn-npc-submit" onClick={() => void executeSpawnNpcSubmission(storeSnap, { selectedNpc, level, place, coords }, places, players)} style={BTN_STYLE}>
          Spawn {selectedNpc}
        </button>
      </div>
    </section>
  );
}

export async function refreshWorldEntitiesAfterNodeTierChange(
  storeSnap: WorldStreamState,
  fetchImpl: typeof fetch = typeof fetch === "function" ? fetch : (undefined as unknown as typeof fetch),
): Promise<void> {
  if (typeof fetchImpl !== "function") return;
  if (typeof fetch === "function" && fetchImpl === fetch) await new Promise((resolve) => setTimeout(resolve, 400));
  const basePollUrl = buildWorldStatePollUrl(storeSnap.layers) ?? "/api/v1/world/state?zone=verra&min_x=-2000000&max_x=2000000&min_y=-2000000&max_y=2000000";
  const pollUrl = `${basePollUrl}${basePollUrl.includes("?") ? "&" : "?"}fresh=1`;
  try {
    const res = await fetchWithTransientRetry(pollUrl, { headers: { Accept: "application/json" } }, { fetchImpl });
    if (!res.ok) return;
    const data = (await res.json()) as Record<string, unknown>;
    if (!Array.isArray(data.entities)) return;
    const rawEntities = data.entities as readonly Record<string, unknown>[];
    const added: WorldEntity[] = rawEntities.map((e) => {
      const rawKind = String(e.kind ?? (e.hostile ? "monster" : "npc"));
      const kind = rawKind === "player" || rawKind === "monster" || rawKind === "resource_node" || rawKind === "mount" || rawKind === "loot_pile" || rawKind === "poi" ? rawKind : "npc";
      const rawName = String(e.name ?? kind);
      const rawSubkind = typeof e.npc_subkind === "string" && e.npc_subkind.length > 0 ? e.npc_subkind : typeof e.npcSubkind === "string" && e.npcSubkind.length > 0 ? e.npcSubkind : undefined;
      const rawResType = typeof e.resourceType === "string" && e.resourceType.length > 0 ? e.resourceType : typeof e.resource_type === "string" && e.resource_type.length > 0 ? e.resource_type : undefined;
      const resType = kind === "resource_node" || rawSubkind === "huntable" || Boolean(rawResType) ? inferResourceNodeType(rawName, rawResType, rawSubkind) : undefined;
      const qual = typeof e.quality === "string" && e.quality.length > 0 ? e.quality : typeof e.rarity === "string" && e.rarity.length > 0 ? e.rarity : kind === "resource_node" ? "Common · Tier 1 (Novice)" : undefined;
      return {
        guid: String(e.id ?? e.guid ?? e.name ?? "entity"), name: rawName, kind,
        xCm: Number(e.x_cm ?? e.x ?? 0), yCm: Number(e.y_cm ?? e.y ?? 0), zCm: Number(e.z_cm ?? e.z ?? 0),
        yawDeg: Number(e.yaw_deg ?? 0), level: Number(e.level ?? 1), healthPct: Number(e.health_pct ?? e.hp ?? 100),
        npc_subkind: rawSubkind, npcSubkind: rawSubkind, resourceType: resType, resource_type: resType, quality: qual,
        zoneId: typeof e.zone === "string" ? e.zone : typeof e.zone_id === "string" ? e.zone_id : undefined,
      };
    });
    storeSnap.applyWorldDelta({ seq: storeSnap.lastSeq + 1, isSnapshot: true, added });
  } catch { /* Non-fatal */ }
}

function filterOnlineMayorCandidates(storeSnap: WorldStreamState, query: string): readonly OnlinePlayerRecord[] {
  const out: OnlinePlayerRecord[] = [], seen = new Set<string>();
  for (const ent of storeSnap.entities.values()) {
    if (ent.kind !== "player") continue;
    const key = (ent.name || ent.guid).toLowerCase();
    if (seen.has(key)) continue;
    seen.add(key);
    out.push({ character_id: ent.guid, name: ent.name || ent.guid, x_cm: ent.xCm, y_cm: ent.yCm, z_cm: ent.zCm, online: true, zone: ent.zoneId });
  }
  const q = query.trim().toLowerCase();
  return q ? out.filter((p) => p.name.toLowerCase().includes(q) || p.character_id.toLowerCase().includes(q)) : out;
}

function renderNodeMayorAndElectionSection(
  node: VerraCityNodeMeta,
  mayorInput: string,
  setMayorInput: (next: string) => void,
  storeSnap: WorldStreamState,
): React.ReactElement {
  const mayorCandidates = filterOnlineMayorCandidates(storeSnap, mayorInput);
  const electionActions: readonly { readonly id: NodeElectionAction; readonly label: string }[] = [
    { id: "start", label: "Start Election" }, { id: "open_voting", label: "Open Voting" },
    { id: "conclude", label: "Conclude Election" }, { id: "cancel", label: "Cancel" },
  ];
  return (
    <div style={{ borderTop: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, paddingTop: "8px" }}>
      <div style={{ marginBottom: "6px" }}>
        Mayor: <strong data-testid="city-node-current-mayor">{node.mayor_name || "Vacant"}</strong> | Election: <strong data-testid="city-node-election-status">{node.election_status || "none"}</strong>
      </div>
      <div style={{ display: "flex", gap: "4px", marginBottom: "6px", flexWrap: "wrap" }}>
        <input type="search" aria-label="Mayor character name or GUID" data-testid="city-node-mayor-input" value={mayorInput} onChange={(e) => setMayorInput(e.currentTarget.value)} placeholder="Search player by character name, ID, or account name..." style={{ ...INPUT_STYLE, flex: 1 }} />
        <button type="button" data-testid="city-node-set-mayor-btn" onClick={() => void storeSnap.setNodeMayor(node.name, mayorInput || storeSnap.selectedPlayer)} style={BTN_STYLE}>Set Mayor</button>
        <button type="button" data-testid="city-node-clear-mayor-btn" onClick={() => void storeSnap.setNodeMayor(node.name, "")} style={CHIP_STYLE}>Clear</button>
      </div>
      {mayorCandidates.length > 0 ? (
        <div data-testid="city-node-mayor-search-results" style={{ maxHeight: "96px", overflowY: "auto", display: "flex", flexDirection: "column", gap: "3px", marginBottom: "6px", padding: "4px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: "rgba(9, 13, 22, 0.65)" }}>
          {mayorCandidates.slice(0, 6).map((pl) => (
            <button key={pl.character_id || pl.name} type="button" data-testid={`city-node-mayor-candidate-${pl.name}`} onClick={() => { setMayorInput(pl.name); storeSnap.setSelectedPlayer(pl.name); }} style={{ display: "flex", justifyContent: "space-between", alignItems: "center", width: "100%", padding: "4px 6px", border: "none", borderRadius: "4px", cursor: "pointer", backgroundColor: mayorInput === pl.name ? "rgba(232, 178, 90, 0.18)" : "transparent", color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100, fontSize: "11px", textAlign: "left" }}>
              <strong>{pl.name}</strong>
              <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>({pl.character_id}{pl.zone ? ` · ${pl.zone}` : ""})</span>
            </button>
          ))}
        </div>
      ) : null}
      <div style={{ display: "grid", gridTemplateColumns: "repeat(4, 1fr)", gap: "4px" }}>
        {electionActions.map((act) => (
          <button key={act.id} type="button" data-testid={`city-node-election-${act.id}`} onClick={() => void storeSnap.triggerNodeElection(node.name, act.id)} style={CHIP_STYLE}>{act.label}</button>
        ))}
      </div>
    </div>
  );
}

export function NodeGovernanceModalCard(props: { readonly storeSnap: WorldStreamState }): React.ReactElement {
  const { storeSnap } = props;
  const [mayorInput, setMayorInput] = React.useState("");
  const node: VerraCityNodeMeta = storeSnap.cityNodes.find((n) => n.name === storeSnap.selectedCityNodeName) ?? storeSnap.cityNodes[0];
  const nodeCenter = getCityNodeCenterCm(node);
  const nodeWorker = resolveWorkerForCoordinateOrZone(nodeCenter.cx_cm, nodeCenter.cy_cm, undefined, storeSnap.workers, node.name);
  return (
    <section aria-label="City Node Stage & Governance Control" data-testid="city-node-stage-editor" style={CARD_STYLE}>
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: "8px", gap: "8px" }}>
        <div>
          <strong style={{ color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 }}>{node.display_name} Governance</strong>
          <div data-testid="city-node-worker-badge" style={{ fontSize: "11px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>{nodeWorker.worker_id} · {nodeWorker.region_label}</div>
        </div>
        <div style={{ display: "flex", gap: "4px", alignItems: "center" }}>
          <select aria-label="Select City Node" data-testid="governance-node-select" value={node.name} onChange={(e) => { const nextName = e.currentTarget.value; storeSnap.selectCityNode(nextName); const found = storeSnap.cityNodes.find((n) => n.name === nextName); if (found) { const { cx_cm, cy_cm } = getCityNodeCenterCm(found); storeSnap.setCameraViewport({ centerXCm: cx_cm, centerYCm: cy_cm, zoom: 5.5 }); } }} style={INPUT_STYLE}>
            {storeSnap.cityNodes.map((n) => (<option key={n.name} value={n.name}>{n.display_name} (Lv {n.level})</option>))}
          </select>
          <button type="button" aria-label="Close city node inspector" onClick={() => storeSnap.selectCityNode(null)} style={CHIP_STYLE}>Close</button>
        </div>
      </div>
      <div style={{ display: "grid", gridTemplateColumns: "repeat(4, 1fr)", gap: "4px", marginBottom: "8px" }}>
        {NODE_STAGE_NAMES.map((stageName, lvl) => (
          <button key={stageName} type="button" data-testid={`set-node-level-${lvl}`} onClick={() => {
            storeSnap.updateCityNodeLevel(node.name, lvl);
            const prose = `Set ${node.display_name} to Stage Lv ${lvl} (${stageName})`;
            void sendGmCommand({ ...buildSetNodeLevelCommand(node.name, lvl), node_guid: node.guid, zone: nodeWorker.scope_key, worker_id: nodeWorker.worker_id } as never)
              .catch((err: unknown) => ({ ok: false, message: err instanceof Error ? err.message : "Network error", correlationId: "" }))
              .then((r) => { storeSnap.setScopedReceipt("nodeLevel", { ok: r.ok, message: r.ok ? prose : `${prose}: ${r.message}`, correlationId: r.correlationId }); if (r.ok) void refreshWorldEntitiesAfterNodeTierChange(storeSnap); });
          }} style={{ ...CHIP_STYLE, backgroundColor: node.level === lvl ? SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: node.level === lvl ? SANDAL_LAUNCHER_TOKENS.VerraObsidian100 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100 }}>
            Lv {lvl} {stageName.slice(0, 6)}
          </button>
        ))}
      </div>
      <div style={{ display: "flex", gap: "6px", marginBottom: "8px", alignItems: "center", flexWrap: "wrap" }}>
        {[1000, 10000].map((xp) => (
          <button key={xp} type="button" data-testid={`add-node-xp-${xp}`} onClick={() => void dispatchAuxiliaryGmAction({ action: "add_node_xp", node_name: node.name, node_guid: node.guid, xp, zone: nodeWorker.scope_key, worker_id: nodeWorker.worker_id }).then((r) => { const prose = `Added +${xp.toLocaleString()} XP to ${node.display_name}`; storeSnap.setScopedReceipt("nodeXp", { ok: r.ok, message: r.ok ? prose : `${prose}: ${r.message}` }); })} style={CHIP_STYLE}>+{xp.toLocaleString()} XP</button>
        ))}
        <button type="button" data-testid="city-node-teleport-here-btn" onClick={() => { const c = getCityNodeCenterCm(node); storeSnap.openCommandModal("teleport", { xCm: c.xCm, yCm: c.yCm, zCm: c.zCm }); }} style={BTN_STYLE}>Teleport Player Here...</button>
        <button type="button" data-testid="city-node-spawn-here-btn" onClick={() => { const c = getCityNodeCenterCm(node); storeSnap.openCommandModal("spawn", { xCm: c.xCm, yCm: c.yCm, zCm: c.zCm }); }} style={CHIP_STYLE}>Spawn NPC / Monster Here...</button>
      </div>
      {renderNodeMayorAndElectionSection(node, mayorInput, setMayorInput, storeSnap)}
    </section>
  );
}

export function renderNodeGovernanceModalCard(storeSnap: WorldStreamState): React.ReactElement {
  return React.createElement(NodeGovernanceModalCard, { storeSnap });
}
