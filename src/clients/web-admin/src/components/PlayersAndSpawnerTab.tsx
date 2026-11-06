// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import { useWorldStreamStore } from "../state/worldStreamStore";
import {
  buildGiveMoneyCommand,
  buildGrantItemCommand,
  buildPaginatedQueryUrl,
  buildSpawnNpcCommand,
  buildTeleportPlayerCommand,
  fetchWithTransientRetry,
  ITEM_RARITIES,
  type ItemRarity,
  normalizePaginatedEnvelope,
  type PaginatedListEnvelope,
  sendGmCommand,
} from "./GmCommandPalette";

export interface OnlinePlayerRecord {
  readonly character_id: string;
  readonly name: string;
  readonly x_cm: number;
  readonly y_cm: number;
  readonly z_cm: number;
  readonly online: boolean;
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
  readonly display_name: string;
  readonly min_level: number;
  readonly max_level: number;
}

export interface CatalogPlaceEntry {
  readonly guid: string;
  readonly name: string;
  readonly display_name: string;
  readonly x_cm: number;
  readonly y_cm: number;
  readonly z_cm: number;
}

export type TeleportMode = "player" | "place" | "coords";
export type SpawnTargetMode = "place" | "coords" | "player" | "picked";

export const DEFAULT_CATALOG_ITEMS: readonly CatalogItemEntry[] = Object.freeze([
  { guid: "6064629100001", name: "Item_Weapon_2H_Sword_Radiant", display_name: "Radiant Greatsword", equip_slot: "MainHand", level: 25 },
  { guid: "6064629100002", name: "Item_Consumable_HealthPotion", display_name: "Major Health Elixir", equip_slot: "Consumable", level: 10 },
  { guid: "6064629100003", name: "Item_Armor_Chest_SteelPlate", display_name: "Riverlands Steel Cuirass", equip_slot: "Chest", level: 20 },
]);

export const DEFAULT_CATALOG_NPCS: readonly CatalogNpcEntry[] = Object.freeze([
  { guid: "6064630100001", name: "DesignData_NPC_Goblin", display_name: "Riverlands Goblin Scout", min_level: 5, max_level: 15 },
  { guid: "6064630100002", name: "DesignData_NPC_Highwayman", display_name: "Winstead Highwayman", min_level: 10, max_level: 20 },
  { guid: "6064630100003", name: "DesignData_NPC_FireDragon", display_name: "Elder Ashen Drake", min_level: 40, max_level: 50 },
]);

export const DEFAULT_CATALOG_PLACES: readonly CatalogPlaceEntry[] = Object.freeze([
  { guid: "108646108518008", name: "Verra_RVR_Winstead", display_name: "Winstead", x_cm: -641389.0, y_cm: 377367.1, z_cm: 12500.0 },
  { guid: "108646121231993", name: "Verra_RVR_Joeva", display_name: "Joeva", x_cm: -1092010.8, y_cm: 522023.2, z_cm: 11200.0 },
  { guid: "108646608033403", name: "Verra_RVR_NewAela", display_name: "New Aela", x_cm: -1134183.54, y_cm: 853387.28, z_cm: 14800.0 },
  { guid: "108646610851452", name: "Verra_RVR_Halcyon", display_name: "Halcyon", x_cm: -892130.8, y_cm: 297711.6, z_cm: 9800.0 },
  { guid: "108646615701117", name: "Verra_RVR_Miraleth", display_name: "Miraleth", x_cm: -459889.63, y_cm: 599317.09, z_cm: 13100.0 },
]);

const CARD_STYLE: React.CSSProperties = {
  padding: "12px",
  borderRadius: "6px",
  border: "1px solid #30363D",
  backgroundColor: "#0D1117",
  color: "#F0F6FC",
  fontSize: "12px",
};

const INPUT_STYLE: React.CSSProperties = {
  padding: "5px 8px",
  borderRadius: "4px",
  border: "1px solid #30363D",
  backgroundColor: "#161B22",
  color: "#F0F6FC",
  fontSize: "12px",
};

const BTN_STYLE: React.CSSProperties = {
  padding: "6px 10px",
  borderRadius: "4px",
  border: "1px solid #30363D",
  backgroundColor: "#1F6FEB",
  color: "#FFFFFF",
  fontSize: "12px",
  fontWeight: 600,
  cursor: "pointer",
};

function renderPaginationControls<T>(
  testIdPrefix: string,
  env: PaginatedListEnvelope<T>,
  onOffsetChange: (nextOffset: number) => void,
): React.ReactElement {
  const canPrev = env.offset > 0;
  const canNext = env.has_more || env.offset + env.limit < env.total;
  return (
    <div data-testid={`${testIdPrefix}-pagination`} style={{ display: "flex", alignItems: "center", justifyContent: "space-between", marginTop: "6px", fontSize: "11px", color: "#8B949E" }}>
      <span>Total: {env.total} (offset {env.offset}, limit {env.limit})</span>
      <div style={{ display: "flex", gap: "4px" }}>
        <button type="button" data-testid={`${testIdPrefix}-prev`} disabled={!canPrev} onClick={() => onOffsetChange(Math.max(0, env.offset - env.limit))} style={{ padding: "2px 8px", borderRadius: "4px", border: "1px solid #30363D", background: "#21262D", color: "#C9D1D9", cursor: canPrev ? "pointer" : "not-allowed" }}>Prev</button>
        <button type="button" data-testid={`${testIdPrefix}-next`} disabled={!canNext} onClick={() => onOffsetChange(env.offset + env.limit)} style={{ padding: "2px 8px", borderRadius: "4px", border: "1px solid #30363D", background: "#21262D", color: "#C9D1D9", cursor: canNext ? "pointer" : "not-allowed" }}>Next</button>
      </div>
    </div>
  );
}

function usePaginatedResource<T>(
  endpoint: string,
  fallbackKey: string,
  defaults: readonly T[],
  q: string,
  offset: number,
): PaginatedListEnvelope<T> {
  const [env, setEnv] = React.useState<PaginatedListEnvelope<T>>({
    items: defaults,
    total: defaults.length,
    limit: 15,
    offset: 0,
    has_more: false,
    next_cursor: null,
  });
  React.useEffect(() => {
    if (typeof fetch !== "function") return;
    let cancelled = false;
    const url = buildPaginatedQueryUrl(endpoint, { q, limit: 15, offset });
    fetchWithTransientRetry(url)
      .then((r) => (r.ok ? r.json() : null))
      .then((raw) => {
        if (cancelled || !raw) return;
        const parsed = normalizePaginatedEnvelope<T>(raw, [fallbackKey], 15, offset);
        if (parsed.items.length > 0 || q.trim().length > 0) setEnv(parsed);
      })
      .catch(() => {});
    return () => {
      cancelled = true;
    };
  }, [endpoint, fallbackKey, q, offset]);
  return env;
}

function renderPlayerRosterCard(
  playerEnv: PaginatedListEnvelope<OnlinePlayerRecord>,
  playerQuery: string,
  selectedPlayer: string,
  onQueryChange: (q: string, offset: number) => void,
  onSelectPlayer: (p: OnlinePlayerRecord) => void,
): React.ReactElement {
  return (
    <section aria-label="Online Players Roster" data-testid="players-roster-card" style={CARD_STYLE}>
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: "8px" }}>
        <strong>Online Players & Target Selection (Player X: {selectedPlayer || "None"})</strong>
        <input type="search" aria-label="Search online players" data-testid="players-search-input" value={playerQuery} onChange={(e) => onQueryChange(e.currentTarget.value, 0)} placeholder="Filter players by name..." style={{ ...INPUT_STYLE, width: "220px" }} />
      </div>
      <table style={{ width: "100%", borderCollapse: "collapse", fontSize: "12px" }}>
        <thead>
          <tr style={{ textAlign: "left", color: "#8B949E", borderBottom: "1px solid #30363D" }}>
            <th style={{ padding: "4px" }}>Name</th>
            <th style={{ padding: "4px" }}>Character ID</th>
            <th style={{ padding: "4px" }}>Coordinates (X, Y, Z cm)</th>
            <th style={{ padding: "4px" }}>Status</th>
          </tr>
        </thead>
        <tbody>
          {playerEnv.items.map((p) => (
            <tr key={p.character_id || p.name} data-testid={`player-row-${p.name}`} onClick={() => onSelectPlayer(p)} style={{ cursor: "pointer", backgroundColor: selectedPlayer === p.name ? "rgba(31, 111, 235, 0.25)" : "transparent", borderBottom: "1px solid #21262D" }}>
              <td style={{ padding: "4px", fontWeight: 600 }}>{p.name}</td>
              <td style={{ padding: "4px", color: "#8B949E" }}>{p.character_id}</td>
              <td style={{ padding: "4px" }}>({Math.round(p.x_cm)}, {Math.round(p.y_cm)}, {Math.round(p.z_cm)})</td>
              <td style={{ padding: "4px", color: p.online !== false ? "#3FB950" : "#8B949E" }}>{p.online !== false ? "Online" : "Offline"}</td>
            </tr>
          ))}
        </tbody>
      </table>
      {renderPaginationControls("players", playerEnv, (next) => onQueryChange(playerQuery, next))}
    </section>
  );
}

function renderTeleportCard(
  playerX: string,
  players: readonly OnlinePlayerRecord[],
  places: readonly CatalogPlaceEntry[],
  pickedPoint: { readonly xCm: number; readonly yCm: number; readonly zCm: number } | null,
  onStatus: (msg: string) => void,
): React.ReactElement {
  const [mode, setMode] = React.useState<TeleportMode>("place");
  const [targetPlayer, setTargetPlayer] = React.useState("");
  const [targetPlace, setTargetPlace] = React.useState(DEFAULT_CATALOG_PLACES[0]?.name ?? "Verra_RVR_Winstead");
  const [coords, setCoords] = React.useState({ x: -641389, y: 377367, z: 12500 });

  const submitTeleport = async (): Promise<void> => {
    try {
      const cmd =
        mode === "player"
          ? buildTeleportPlayerCommand({ character: playerX, target_player: targetPlayer })
          : mode === "place"
            ? buildTeleportPlayerCommand({ character: playerX, place: targetPlace })
            : buildTeleportPlayerCommand({ character: playerX, x_cm: coords.x, y_cm: coords.y, z_cm: coords.z });
      const res = await sendGmCommand(cmd);
      onStatus(res.message);
    } catch (err) {
      onStatus((err as Error).message);
    }
  };

  return (
    <section aria-label="Teleport Player Card" data-testid="teleport-player-card" style={CARD_STYLE}>
      <strong style={{ display: "block", marginBottom: "8px" }}>Teleport Player ({playerX || "Select Player X"})</strong>
      <div style={{ display: "flex", gap: "6px", marginBottom: "8px" }}>
        {(["player", "place", "coords"] as const).map((m) => (
          <button key={m} type="button" data-testid={`teleport-mode-${m}`} onClick={() => setMode(m)} style={{ ...INPUT_STYLE, backgroundColor: mode === m ? "#1F6FEB" : "#21262D", cursor: "pointer" }}>
            {m === "player" ? "To Player Y" : m === "place" ? "To Place / Node" : "To (X, Y, Z)"}
          </button>
        ))}
      </div>
      {mode === "player" ? (
        <input type="text" list="online-players-list" aria-label="Target Player Y" data-testid="teleport-target-player" value={targetPlayer} onChange={(e) => setTargetPlayer(e.currentTarget.value)} placeholder="Target Player Y..." style={{ ...INPUT_STYLE, width: "100%", marginBottom: "8px" }} />
      ) : mode === "place" ? (
        <select aria-label="Target Place or City Node" data-testid="teleport-target-place" value={targetPlace} onChange={(e) => setTargetPlace(e.currentTarget.value)} style={{ ...INPUT_STYLE, width: "100%", marginBottom: "8px" }}>
          {places.map((pl) => (
            <option key={pl.name} value={pl.name}>{pl.display_name} ({pl.name})</option>
          ))}
        </select>
      ) : (
        <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr 1fr auto", gap: "4px", marginBottom: "8px" }}>
          <input type="number" aria-label="Teleport X cm" data-testid="teleport-x" value={coords.x} onChange={(e) => setCoords({ ...coords, x: Number(e.currentTarget.value) })} style={INPUT_STYLE} />
          <input type="number" aria-label="Teleport Y cm" data-testid="teleport-y" value={coords.y} onChange={(e) => setCoords({ ...coords, y: Number(e.currentTarget.value) })} style={INPUT_STYLE} />
          <input type="number" aria-label="Teleport Z cm" data-testid="teleport-z" value={coords.z} onChange={(e) => setCoords({ ...coords, z: Number(e.currentTarget.value) })} style={INPUT_STYLE} />
          <button type="button" data-testid="teleport-use-picked" onClick={() => pickedPoint && setCoords({ x: Math.round(pickedPoint.xCm), y: Math.round(pickedPoint.yCm), z: Math.round(pickedPoint.zCm) })} style={{ ...INPUT_STYLE, cursor: "pointer" }}>Use Picked Map Point</button>
        </div>
      )}
      <datalist id="online-players-list">{players.map((p) => <option key={p.name} value={p.name} />)}</datalist>
      <button type="button" data-testid="teleport-submit-btn" onClick={() => void submitTeleport()} style={BTN_STYLE}>Teleport Player</button>
    </section>
  );
}

function renderGiveGoldAndItemCards(
  playerX: string,
  itemEnv: PaginatedListEnvelope<CatalogItemEntry>,
  itemQuery: string,
  onItemQueryChange: (q: string, offset: number) => void,
  onStatus: (msg: string) => void,
): React.ReactElement {
  const [gold, setGold] = React.useState(10);
  const [silver, setSilver] = React.useState(0);
  const [copper, setCopper] = React.useState(0);
  const [selectedItem, setSelectedItem] = React.useState(DEFAULT_CATALOG_ITEMS[0]?.guid ?? "6064629100001");
  const [count, setCount] = React.useState(1);
  const [rarity, setRarity] = React.useState<ItemRarity>("Common");

  return (
    <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: "12px" }}>
      <section aria-label="Give Gold Card" data-testid="give-gold-card" style={CARD_STYLE}>
        <strong style={{ display: "block", marginBottom: "8px" }}>Give Gold ({playerX || "Select Player X"})</strong>
        <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr 1fr", gap: "6px", marginBottom: "8px" }}>
          <label>Gold<input type="number" min={0} data-testid="give-gold-input" value={gold} onChange={(e) => setGold(Number(e.currentTarget.value))} style={{ ...INPUT_STYLE, width: "100%" }} /></label>
          <label>Silver<input type="number" min={0} data-testid="give-silver-input" value={silver} onChange={(e) => setSilver(Number(e.currentTarget.value))} style={{ ...INPUT_STYLE, width: "100%" }} /></label>
          <label>Copper<input type="number" min={0} data-testid="give-copper-input" value={copper} onChange={(e) => setCopper(Number(e.currentTarget.value))} style={{ ...INPUT_STYLE, width: "100%" }} /></label>
        </div>
        <button type="button" data-testid="give-gold-submit" onClick={() => void sendGmCommand(buildGiveMoneyCommand(playerX, gold, silver, copper)).then((r) => onStatus(r.message)).catch((e: Error) => onStatus(e.message))} style={BTN_STYLE}>Give Currency</button>
      </section>

      <section aria-label="Give Item Card" data-testid="give-item-card" style={CARD_STYLE}>
        <strong style={{ display: "block", marginBottom: "8px" }}>Give Item (Catalog Search)</strong>
        <input type="search" aria-label="Search item catalog" data-testid="catalog-items-search" value={itemQuery} onChange={(e) => onItemQueryChange(e.currentTarget.value, 0)} placeholder="Search items by name or GUID..." style={{ ...INPUT_STYLE, width: "100%", marginBottom: "6px" }} />
        <select aria-label="Select Catalog Item" data-testid="catalog-items-select" value={selectedItem} onChange={(e) => setSelectedItem(e.currentTarget.value)} style={{ ...INPUT_STYLE, width: "100%", marginBottom: "6px" }}>
          {itemEnv.items.map((it) => (
            <option key={it.guid} value={it.guid}>{it.display_name} [{it.equip_slot} Lv{it.level}] ({it.guid})</option>
          ))}
        </select>
        <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr auto", gap: "6px", alignItems: "end" }}>
          <label>Qty (1..100)<input type="number" min={1} max={100} data-testid="give-item-qty" value={count} onChange={(e) => setCount(Number(e.currentTarget.value))} style={{ ...INPUT_STYLE, width: "100%" }} /></label>
          <label>Rarity<select data-testid="give-item-rarity" value={rarity} onChange={(e) => setRarity(e.currentTarget.value as ItemRarity)} style={{ ...INPUT_STYLE, width: "100%" }}>{ITEM_RARITIES.map((r) => <option key={r} value={r}>{r}</option>)}</select></label>
          <button type="button" data-testid="give-item-submit" onClick={() => void sendGmCommand(buildGrantItemCommand(playerX, selectedItem, count, rarity)).then((r) => onStatus(r.message)).catch((e: Error) => onStatus(e.message))} style={BTN_STYLE}>Grant Item</button>
        </div>
        {renderPaginationControls("items", itemEnv, (next) => onItemQueryChange(itemQuery, next))}
      </section>
    </div>
  );
}

function renderSpawnNpcCard(
  npcEnv: PaginatedListEnvelope<CatalogNpcEntry>,
  npcQuery: string,
  places: readonly CatalogPlaceEntry[],
  onNpcQueryChange: (q: string, offset: number) => void,
  onStatus: (msg: string) => void,
): React.ReactElement {
  const storeSnap = useWorldStreamStore();
  const [selectedNpc, setSelectedNpc] = React.useState(DEFAULT_CATALOG_NPCS[0]?.name ?? "DesignData_NPC_Goblin");
  const [level, setLevel] = React.useState(15);
  const [targetMode, setTargetMode] = React.useState<SpawnTargetMode>("place");
  const [place, setPlace] = React.useState(DEFAULT_CATALOG_PLACES[0]?.name ?? "Verra_RVR_Winstead");
  const [coords, setCoords] = React.useState({ x: -641389, y: 377367, z: 12500 });

  const handleSpawn = async (): Promise<void> => {
    try {
      const picked = storeSnap.pickedCoordinate;
      const cmd =
        targetMode === "place"
          ? buildSpawnNpcCommand({ npc: selectedNpc, place, level })
          : targetMode === "picked" && picked
            ? buildSpawnNpcCommand({ npc: selectedNpc, x_cm: picked.xCm, y_cm: picked.yCm, z_cm: picked.zCm, level })
            : buildSpawnNpcCommand({ npc: selectedNpc, x_cm: coords.x, y_cm: coords.y, z_cm: coords.z, level });
      const res = await sendGmCommand(cmd);
      onStatus(res.message);
    } catch (err) {
      onStatus((err as Error).message);
    }
  };

  return (
    <section aria-label="Spawn NPC Card" data-testid="spawn-npc-card" style={CARD_STYLE}>
      <strong style={{ display: "block", marginBottom: "8px" }}>Spawn NPC (Searchable Catalog -&gt; Place or Coordinates)</strong>
      <div style={{ display: "grid", gridTemplateColumns: "2fr 2fr 1fr", gap: "6px", marginBottom: "8px" }}>
        <input type="search" aria-label="Search NPC catalog" data-testid="catalog-npcs-search" value={npcQuery} onChange={(e) => onNpcQueryChange(e.currentTarget.value, 0)} placeholder="Search NPCs by name or GUID..." style={INPUT_STYLE} />
        <select aria-label="Select Catalog NPC" data-testid="catalog-npcs-select" value={selectedNpc} onChange={(e) => setSelectedNpc(e.currentTarget.value)} style={INPUT_STYLE}>
          {npcEnv.items.map((n) => (
            <option key={n.guid} value={n.name}>{n.display_name} (Lv {n.min_level}-{n.max_level}) [{n.name}]</option>
          ))}
        </select>
        <input type="number" min={1} max={50} aria-label="NPC Level" data-testid="spawn-npc-level" value={level} onChange={(e) => setLevel(Number(e.currentTarget.value))} style={INPUT_STYLE} />
      </div>
      <div style={{ display: "flex", gap: "6px", marginBottom: "8px", flexWrap: "wrap" }}>
        {(["place", "coords", "picked"] as const).map((m) => (
          <button key={m} type="button" data-testid={`spawn-mode-${m}`} onClick={() => setTargetMode(m)} style={{ ...INPUT_STYLE, backgroundColor: targetMode === m ? "#1F6FEB" : "#21262D", cursor: "pointer" }}>
            {m === "place" ? "At Place / Node" : m === "picked" ? "At Picked Map Point" : "At (X, Y, Z)"}
          </button>
        ))}
        {targetMode === "place" ? (
          <select aria-label="Spawn Place" data-testid="spawn-npc-place" value={place} onChange={(e) => setPlace(e.currentTarget.value)} style={INPUT_STYLE}>
            {places.map((pl) => <option key={pl.name} value={pl.name}>{pl.display_name}</option>)}
          </select>
        ) : (
          <>
            <input type="number" aria-label="Spawn X cm" data-testid="spawn-npc-x" value={coords.x} onChange={(e) => setCoords({ ...coords, x: Number(e.currentTarget.value) })} style={INPUT_STYLE} />
            <input type="number" aria-label="Spawn Y cm" data-testid="spawn-npc-y" value={coords.y} onChange={(e) => setCoords({ ...coords, y: Number(e.currentTarget.value) })} style={INPUT_STYLE} />
            <input type="number" aria-label="Spawn Z cm" data-testid="spawn-npc-z" value={coords.z} onChange={(e) => setCoords({ ...coords, z: Number(e.currentTarget.value) })} style={INPUT_STYLE} />
          </>
        )}
        <button type="button" data-testid="spawn-npc-submit" onClick={() => void handleSpawn()} style={BTN_STYLE}>Spawn NPC</button>
      </div>
      {renderPaginationControls("npcs", npcEnv, (next) => onNpcQueryChange(npcQuery, next))}
    </section>
  );
}

export function PlayersAndSpawnerTab(): React.ReactElement {
  const storeSnap = useWorldStreamStore();
  const [playerQuery, setPlayerQuery] = React.useState({ q: "", offset: 0 });
  const [itemQuery, setItemQuery] = React.useState({ q: "", offset: 0 });
  const [npcQuery, setNpcQuery] = React.useState({ q: "", offset: 0 });
  const [statusMessage, setStatusMessage] = React.useState<string | null>(null);

  const defaultPlayers: readonly OnlinePlayerRecord[] = [
    { character_id: "char-101", name: "Kaelen", x_cm: -641389, y_cm: 377367, z_cm: 12500, online: true },
    { character_id: "char-102", name: "Lyra", x_cm: -1092010, y_cm: 522023, z_cm: 11200, online: true },
  ];
  const playerEnv = usePaginatedResource<OnlinePlayerRecord>("/api/v1/world/players", "players", defaultPlayers, playerQuery.q, playerQuery.offset);
  const itemEnv = usePaginatedResource<CatalogItemEntry>("/api/v1/catalog/items", "items", DEFAULT_CATALOG_ITEMS, itemQuery.q, itemQuery.offset);
  const npcEnv = usePaginatedResource<CatalogNpcEntry>("/api/v1/catalog/npcs", "npcs", DEFAULT_CATALOG_NPCS, npcQuery.q, npcQuery.offset);
  const placeEnv = usePaginatedResource<CatalogPlaceEntry>("/api/v1/catalog/places", "places", DEFAULT_CATALOG_PLACES, "", 0);
  const activePlayer = storeSnap.selectedPlayer || playerEnv.items[0]?.name || "Kaelen";

  return (
    <div data-testid="players-and-spawner-tab" style={{ display: "flex", flexDirection: "column", gap: "12px", padding: "12px", backgroundColor: "#090D16", color: "#F0F6FC", minHeight: "540px" }}>
      {statusMessage ? (
        <div role="status" data-testid="gm-action-status-banner" style={{ padding: "8px 12px", borderRadius: "6px", border: "1px solid #1F6FEB", backgroundColor: "rgba(31, 111, 235, 0.15)", color: "#58A6FF", fontSize: "12px" }}>
          {statusMessage}
        </div>
      ) : null}
      {renderPlayerRosterCard(playerEnv, playerQuery.q, activePlayer, (q, offset) => setPlayerQuery({ q, offset }), (p) => storeSnap.setSelectedPlayer(p.name))}
      {renderTeleportCard(activePlayer, playerEnv.items, placeEnv.items, storeSnap.pickedCoordinate, setStatusMessage)}
      {renderGiveGoldAndItemCards(activePlayer, itemEnv, itemQuery.q, (q, offset) => setItemQuery({ q, offset }), setStatusMessage)}
      {renderSpawnNpcCard(npcEnv, npcQuery.q, placeEnv.items, (q, offset) => setNpcQuery({ q, offset }), setStatusMessage)}
    </div>
  );
}

export default PlayersAndSpawnerTab;
