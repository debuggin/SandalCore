// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import { createPortal } from "react-dom";
import {
  type GmModalId,
  resolveWorkerForCoordinateOrZone,
  type SpawnTargetSubMode,
  type TeleportSubMode,
  useWorldStreamStore,
  type WorldStreamState,
} from "../state/worldStreamStore";
import {
  buildGiveMoneyCommand,
  buildGrantItemCommand,
  buildPaginatedQueryUrl,
  buildTeleportPlayerCommand,
  fetchWithTransientRetry,
  type GmActionCommandPayload,
  ITEM_RARITIES,
  type ItemRarity,
  normalizePaginatedEnvelope,
  type PaginatedListEnvelope,
  sendGmCommand,
} from "./GmCommandPalette";
import {
  CatalogComboboxPicker,
  type CatalogComboboxPickerProps,
  type CatalogItemEntry,
  type CatalogNpcEntry,
  type CatalogPickerOption,
  type CatalogPlaceEntry,
  DEFAULT_CATALOG_ITEMS,
  DEFAULT_CATALOG_NPCS,
  DEFAULT_CATALOG_PLACES,
  DEFAULT_ONLINE_PLAYERS,
  dispatchAuxiliaryGmAction,
  MOVE_SPEED_MULTIPLIERS,
  NodeGovernanceModalCard,
  type OnlinePlayerRecord,
  refreshWorldEntitiesAfterNodeTierChange,
  renderNodeGovernanceModalCard,
  SANDAL_LAUNCHER_TOKENS,
  SpawnNpcCard,
} from "./NodeGovernanceAndSpawnModals";

export {
  CatalogComboboxPicker,
  type CatalogComboboxPickerProps,
  type CatalogItemEntry,
  type CatalogNpcEntry,
  type CatalogPickerOption,
  type CatalogPlaceEntry,
  DEFAULT_CATALOG_ITEMS,
  DEFAULT_CATALOG_NPCS,
  DEFAULT_CATALOG_PLACES,
  DEFAULT_ONLINE_PLAYERS,
  dispatchAuxiliaryGmAction,
  MOVE_SPEED_MULTIPLIERS,
  NodeGovernanceModalCard,
  type OnlinePlayerRecord,
  refreshWorldEntitiesAfterNodeTierChange,
  renderNodeGovernanceModalCard,
  SANDAL_LAUNCHER_TOKENS,
  SpawnNpcCard,
};

export interface RecentGmRunEntry {
  readonly id: string;
  readonly summary: string;
  readonly payload: GmActionCommandPayload;
  readonly scope: "currency" | "giveItem" | "teleport" | "spawnNpc";
}

export type TeleportMode = TeleportSubMode;
export type SpawnTargetMode = SpawnTargetSubMode;

const CARD_STYLE: React.CSSProperties = {
  padding: "14px",
  borderRadius: "10px",
  border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
  background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush,
  backdropFilter: "blur(16px)",
  boxShadow: "0 12px 32px rgba(0, 0, 0, 0.55)",
  color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100,
  fontSize: "12px",
};

const INPUT_STYLE: React.CSSProperties = {
  minHeight: "32px",
  padding: "5px 8px",
  borderRadius: "4px",
  border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
  backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush,
  color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100,
  fontSize: "12px",
};

const CHIP_STYLE: React.CSSProperties = {
  minHeight: "32px",
  padding: "4px 10px",
  borderRadius: "999px",
  border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
  backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush,
  color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100,
  fontSize: "11px",
  fontWeight: 600,
  cursor: "pointer",
};

const BTN_STYLE: React.CSSProperties = {
  minHeight: "32px",
  padding: "6px 14px",
  borderRadius: "4px",
  border: `1px solid ${SANDAL_LAUNCHER_TOKENS.VerraBorder400}`,
  background: "linear-gradient(135deg, #E8B25A 0%, #B8842D 100%)",
  color: SANDAL_LAUNCHER_TOKENS.VerraObsidian100,
  fontSize: "12px",
  fontWeight: 700,
  cursor: "pointer",
};

export function shouldUpdatePaginatedResourceState<T>(
  fallbackKey: string,
  parsed: PaginatedListEnvelope<T>,
  query: string,
): boolean {
  return (
    parsed.items.length > 0 ||
    query.trim().length > 0 ||
    fallbackKey === "players" ||
    fallbackKey === "items" ||
    fallbackKey === "npcs"
  );
}

export function usePaginatedResource<T>(
  endpoint: string,
  fallbackKey: string,
  defaults: readonly T[],
  q: string,
): PaginatedListEnvelope<T> {
  const [env, setEnv] = React.useState<PaginatedListEnvelope<T>>({
    items: defaults,
    total: defaults.length,
    limit: 25,
    offset: 0,
    has_more: false,
    next_cursor: null,
  });
  React.useEffect(() => {
    if (typeof fetch !== "function") return;
    let cancelled = false;
    const timer = setTimeout(() => {
      const url = buildPaginatedQueryUrl(endpoint, { q, limit: 25, offset: 0 });
      fetchWithTransientRetry(url)
        .then((r) => (r.ok ? r.json() : null))
        .then((raw) => {
          if (cancelled || !raw) return;
          const parsed = normalizePaginatedEnvelope<T>(raw, [fallbackKey], 25, 0);
          if (shouldUpdatePaginatedResourceState(fallbackKey, parsed, q)) setEnv(parsed);
        })
        .catch(() => {});
    }, 120);
    return () => {
      cancelled = true;
      clearTimeout(timer);
    };
  }, [endpoint, fallbackKey, q]);
  return env;
}

export interface TeleportCardProps {
  readonly playerX: string;
  readonly players: readonly OnlinePlayerRecord[];
  readonly places: readonly CatalogPlaceEntry[];
  readonly storeSnap: WorldStreamState;
  readonly onRecordRun: (entry: RecentGmRunEntry) => void;
}

export function mergeAndFilterOnlinePlayers(
  players: readonly OnlinePlayerRecord[],
  storeSnap: WorldStreamState,
  query: string,
): readonly OnlinePlayerRecord[] {
  const byKey = new Map<string, OnlinePlayerRecord>();
  for (const pl of players) {
    byKey.set((pl.name || pl.character_id).toLowerCase(), pl);
  }
  for (const ent of storeSnap.entities.values()) {
    if (ent.kind !== "player") continue;
    const key = (ent.name || ent.guid).toLowerCase();
    if (!byKey.has(key)) {
      byKey.set(key, {
        character_id: ent.guid,
        name: ent.name || ent.guid,
        x_cm: ent.xCm,
        y_cm: ent.yCm,
        z_cm: ent.zCm,
        online: true,
        zone: ent.zoneId,
      });
    }
  }
  const q = query.trim().toLowerCase();
  const merged = Array.from(byKey.values());
  if (!q) return merged;
  return merged.filter(
    (p) =>
      p.name.toLowerCase().includes(q) ||
      p.character_id.toLowerCase().includes(q) ||
      ((p as { account?: string }).account ?? "").toLowerCase().includes(q),
  );
}

interface TeleportDispatchArgs {
  readonly character: string;
  readonly mode: TeleportSubMode;
  readonly targetPlayer: string;
  readonly operatorChar: string;
  readonly targetPlace: string;
  readonly coords: { readonly x: number; readonly y: number; readonly z: number };
  readonly players: readonly OnlinePlayerRecord[];
  readonly places: readonly CatalogPlaceEntry[];
  readonly storeSnap: WorldStreamState;
  readonly onRecordRun: (entry: RecentGmRunEntry) => void;
}

async function dispatchTeleportAction(args: TeleportDispatchArgs): Promise<void> {
  const { character, mode, targetPlayer, operatorChar, targetPlace, coords, players, places, storeSnap, onRecordRun } = args;
  try {
    const matchedPlace = places.find((pl) => pl.name === targetPlace);
    const matchedPlayer = players.find((pl) => pl.name === (mode === "player" ? targetPlayer : character));
    const tx = mode === "place" ? (matchedPlace?.x_cm ?? coords.x) : mode === "coords" ? coords.x : (matchedPlayer?.x_cm ?? coords.x);
    const ty = mode === "place" ? (matchedPlace?.y_cm ?? coords.y) : mode === "coords" ? coords.y : (matchedPlayer?.y_cm ?? coords.y);
    const worker = resolveWorkerForCoordinateOrZone(tx, ty, matchedPlayer?.zone, undefined, mode === "place" ? targetPlace : undefined);
    const baseCmd =
      mode === "player"
        ? buildTeleportPlayerCommand({ character, target_player: targetPlayer })
        : mode === "to_player"
          ? buildTeleportPlayerCommand({ character: operatorChar || character, target_player: character })
          : mode === "place"
            ? buildTeleportPlayerCommand({ character, place: targetPlace })
            : buildTeleportPlayerCommand({ character, x_cm: coords.x, y_cm: coords.y, z_cm: coords.z });
    const cmd = { ...baseCmd, zone: worker.scope_key, worker_id: worker.worker_id };
    const res = await sendGmCommand(cmd);
    const placeLabel = matchedPlace?.display_name ?? targetPlace;
    const prose =
      mode === "player"
        ? `Teleported ${character} to player ${targetPlayer}`
        : mode === "to_player"
          ? `Teleported ${operatorChar || "operator"} to player ${character}`
          : mode === "place"
            ? `Teleported ${character} to ${placeLabel}`
            : `Teleported ${character} to (${coords.x}, ${coords.y}, ${coords.z})`;
    storeSnap.setScopedReceipt("teleport", { ok: res.ok, message: res.ok ? prose : `${prose}: ${res.message}`, correlationId: res.correlationId });
    onRecordRun({ id: `${Date.now()}-tp`, summary: prose, payload: cmd, scope: "teleport" });
  } catch (err) {
    storeSnap.setScopedReceipt("teleport", { ok: false, message: (err as Error).message });
  }
}

function renderTeleportPlayerSearchSection(
  playerSearch: string,
  activeChar: string,
  filteredPlayers: readonly OnlinePlayerRecord[],
  onSelectOrSearch: (nextPlayer: string) => void,
  onQuickTeleportHere: (playerName: string) => void,
): React.ReactElement {
  return (
    <div style={{ marginBottom: "8px" }}>
      <input
        type="search"
        aria-label="Search online players"
        data-testid="players-search-input"
        value={playerSearch}
        onChange={(e) => onSelectOrSearch(e.currentTarget.value)}
        placeholder="Search player to teleport by character name, ID, or account name..."
        style={{ ...INPUT_STYLE, width: "100%", boxSizing: "border-box", marginBottom: "6px" }}
      />
      {filteredPlayers.length > 0 ? (
        <div
          data-testid="teleport-player-search-results"
          style={{ maxHeight: "110px", overflowY: "auto", display: "flex", flexDirection: "column", gap: "4px", padding: "4px", borderRadius: "4px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: "rgba(9, 13, 22, 0.65)" }}
        >
          {filteredPlayers.slice(0, 8).map((pl) => (
            <div
              key={pl.character_id || pl.name}
              onClick={() => onSelectOrSearch(pl.name)}
              style={{ display: "flex", justifyContent: "space-between", alignItems: "center", padding: "4px 6px", borderRadius: "4px", cursor: "pointer", backgroundColor: activeChar === pl.name ? "rgba(232, 178, 90, 0.18)" : "transparent" }}
            >
              <span>
                <strong>{pl.name}</strong>{" "}
                <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, fontSize: "11px" }}>
                  ({pl.character_id}{pl.zone ? ` · ${pl.zone}` : ""})
                </span>
              </span>
              <button
                type="button"
                data-testid={`teleport-row-here-${pl.name}`}
                onClick={(ev) => {
                  ev.stopPropagation();
                  onQuickTeleportHere(pl.name);
                }}
                style={{ ...CHIP_STYLE, minHeight: "24px", padding: "2px 8px" }}
              >
                Teleport Here
              </button>
            </div>
          ))}
        </div>
      ) : null}
    </div>
  );
}

function renderTeleportModeInputs(
  mode: TeleportSubMode,
  placeOpts: readonly CatalogPickerOption[],
  state: {
    readonly targetPlace: string;
    readonly placeText: string;
    readonly targetPlayer: string;
    readonly operatorChar: string;
    readonly activeChar: string;
    readonly coords: { readonly x: number; readonly y: number; readonly z: number };
    readonly pin: { readonly xCm?: number; readonly yCm?: number; readonly zCm?: number } | null;
  },
  handlers: {
    readonly setPlaceText: (v: string) => void;
    readonly setTargetPlace: (v: string) => void;
    readonly setTargetPlayer: (v: string) => void;
    readonly setOperatorChar: (v: string) => void;
    readonly setCoords: (c: { readonly x: number; readonly y: number; readonly z: number }) => void;
  },
): React.ReactElement {
  const { targetPlace, placeText, targetPlayer, operatorChar, activeChar, coords, pin } = state;
  if (mode === "place") {
    return <CatalogComboboxPicker label="Destination Named Place" testIdPrefix="teleport-place" placeholder="Type place name..." value={targetPlace} text={placeText} options={placeOpts} onTextChange={handlers.setPlaceText} onSelectOption={(o) => { handlers.setTargetPlace(o.value); handlers.setPlaceText(o.label); }} />;
  }
  if (mode === "player") {
    return <input type="search" aria-label="Destination Player" data-testid="teleport-target-player" value={targetPlayer} onChange={(e) => handlers.setTargetPlayer(e.currentTarget.value)} placeholder="Search destination player by character name, ID, or account name..." style={{ ...INPUT_STYLE, width: "100%", boxSizing: "border-box", marginBottom: "6px" }} />;
  }
  if (mode === "to_player") {
    return <input type="search" aria-label="Operator Character" data-testid="teleport-operator-char" value={operatorChar} onChange={(e) => handlers.setOperatorChar(e.currentTarget.value)} placeholder="Search destination player by character name, ID, or account name..." style={{ ...INPUT_STYLE, width: "100%", boxSizing: "border-box", marginBottom: "6px" }} />;
  }
  return (
    <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr 1fr auto", gap: "4px", marginBottom: "8px" }}>
      <input type="number" aria-label="Teleport X cm" data-testid="teleport-x" value={coords.x} onChange={(e) => handlers.setCoords({ ...coords, x: Number(e.currentTarget.value) })} style={INPUT_STYLE} />
      <input type="number" aria-label="Teleport Y cm" data-testid="teleport-y" value={coords.y} onChange={(e) => handlers.setCoords({ ...coords, y: Number(e.currentTarget.value) })} style={INPUT_STYLE} />
      <input type="number" aria-label="Teleport Z cm" data-testid="teleport-z" value={coords.z} onChange={(e) => handlers.setCoords({ ...coords, z: Number(e.currentTarget.value) })} style={INPUT_STYLE} />
      <button type="button" data-testid="teleport-use-picked" onClick={() => pin && handlers.setCoords({ x: Math.round(Number(pin.xCm ?? 0)), y: Math.round(Number(pin.yCm ?? 0)), z: Math.round(Number(pin.zCm ?? 0)) })} style={CHIP_STYLE}>Use Map Pin</button>
    </div>
  );
}

export function TeleportCard(props: TeleportCardProps): React.ReactElement {
  const { playerX, players, places, storeSnap, onRecordRun } = props;
  const mode = storeSnap.teleportMode;
  const [playerSearch, setPlayerSearch] = React.useState(storeSnap.selectedPlayer || playerX);
  const [targetPlayer, setTargetPlayer] = React.useState("");
  const [operatorChar, setOperatorChar] = React.useState("");
  const [placeText, setPlaceText] = React.useState("Winstead");
  const [targetPlace, setTargetPlace] = React.useState(DEFAULT_CATALOG_PLACES[2]?.name ?? "Verra_RVR_Winstead");
  const selectedEntity = storeSnap.selectedEntityGuid ? storeSnap.entities.get(storeSnap.selectedEntityGuid) ?? null : null;
  const selectedPoi = storeSnap.selectedPoiGuid ? storeSnap.pois.find((p) => p.guid === storeSnap.selectedPoiGuid) ?? null : null;
  const pin = storeSnap.lastClickedPoint ?? storeSnap.pickedCoordinate ?? (selectedEntity ? { xCm: selectedEntity.xCm, yCm: selectedEntity.yCm, zCm: selectedEntity.zCm } : selectedPoi ? { xCm: selectedPoi.x_cm, yCm: selectedPoi.y_cm, zCm: selectedPoi.z_cm } : null);
  const [coords, setCoords] = React.useState({ x: Math.round(Number(pin?.xCm ?? -644519)), y: Math.round(Number(pin?.yCm ?? 376773)), z: Math.round(Number(pin?.zCm ?? 12012)) });
  React.useEffect(() => {
    if (pin) setCoords({ x: Math.round(Number(pin.xCm ?? 0)), y: Math.round(Number(pin.yCm ?? 0)), z: Math.round(Number(pin.zCm ?? 0)) });
  }, [pin?.xCm, pin?.yCm, pin?.zCm]);
  const activeChar = (storeSnap.selectedPlayer || playerSearch || playerX).trim();
  const filteredPlayers = mergeAndFilterOnlinePlayers(players, storeSnap, playerSearch);
  const receipt = storeSnap.lastReceiptsByScope.teleport;
  const allPlaceOpts: readonly CatalogPickerOption[] = places.map((pl) => ({ value: pl.name, label: pl.display_name, detail: pl.name }));
  const placeQ = placeText.trim().toLowerCase();
  const placeOpts: readonly CatalogPickerOption[] = placeQ && !allPlaceOpts.some((o) => o.label.toLowerCase() === placeQ) ? allPlaceOpts.filter((o) => o.label.toLowerCase().includes(placeQ) || o.value.toLowerCase().includes(placeQ)) : allPlaceOpts;

  return (
    <section aria-label="Teleport Player Card" data-testid="teleport-player-card" style={CARD_STYLE}>
      <strong style={{ display: "block", marginBottom: "8px", color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 }}>Teleport Player ({activeChar || "Select Player"})</strong>
      {renderTeleportPlayerSearchSection(playerSearch, activeChar, filteredPlayers, (next) => { setPlayerSearch(next); storeSnap.setSelectedPlayer(next); }, (rowPlayer) => {
        setPlayerSearch(rowPlayer);
        storeSnap.setSelectedPlayer(rowPlayer);
        void dispatchTeleportAction({ character: rowPlayer, mode: "coords", targetPlayer, operatorChar, targetPlace, coords, players: filteredPlayers, places, storeSnap, onRecordRun });
      })}
      <div style={{ display: "flex", gap: "6px", marginBottom: "8px", flexWrap: "wrap" }}>
        {([{ id: "place", label: "Player -> Named Place" }, { id: "player", label: "Player -> Destination Player" }, { id: "coords", label: "Player -> Map Location (X, Y, Z)" }] as const).map((m) => (
          <button key={m.id} type="button" data-testid={`teleport-mode-${m.id}`} onClick={() => storeSnap.setTeleportMode(m.id)} style={{ ...CHIP_STYLE, backgroundColor: mode === m.id ? SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: mode === m.id ? SANDAL_LAUNCHER_TOKENS.VerraObsidian100 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100 }}>{m.label}</button>
        ))}
      </div>
      {renderTeleportModeInputs(mode, placeOpts, { targetPlace, placeText, targetPlayer, operatorChar, activeChar, coords, pin }, { setPlaceText, setTargetPlace, setTargetPlayer, setOperatorChar, setCoords })}
      <div style={{ display: "flex", alignItems: "center", gap: "8px", flexWrap: "wrap" }}>
        <button type="button" data-testid="teleport-submit-btn" onClick={() => void dispatchTeleportAction({ character: activeChar, mode, targetPlayer, operatorChar, targetPlace, coords, players: filteredPlayers, places, storeSnap, onRecordRun })} style={BTN_STYLE}>Teleport Player</button>
        {filteredPlayers.length > 0 && mode === "player" ? <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, fontSize: "11px" }}>Online: {filteredPlayers.map((p) => p.name).join(", ")}</span> : null}
        {receipt ? <span role="status" data-testid="teleport-receipt-banner" style={{ color: receipt.ok ? SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500 : SANDAL_LAUNCHER_TOKENS.VerraRuinCrimson500, fontSize: "11px" }}>{receipt.message}</span> : null}
      </div>
    </section>
  );
}

export interface GiveGoldAndItemCardsProps {
  readonly playerX: string;
  readonly itemEnv: PaginatedListEnvelope<CatalogItemEntry>;
  readonly itemQuery: string;
  readonly onItemQueryChange: (q: string) => void;
  readonly onRecordRun: (entry: RecentGmRunEntry) => void;
}

export function GiveGoldAndItemCards(props: GiveGoldAndItemCardsProps): React.ReactElement {
  const { playerX, itemEnv, itemQuery, onItemQueryChange, onRecordRun } = props;
  const storeSnap = useWorldStreamStore();
  const [gold, setGold] = React.useState(10);
  const [silver, setSilver] = React.useState(0);
  const [copper, setCopper] = React.useState(0);
  const [selectedItem, setSelectedItem] = React.useState(DEFAULT_CATALOG_ITEMS[0]?.guid ?? "6064629100001");
  const [count, setCount] = React.useState(1);
  const [rarity, setRarity] = React.useState<ItemRarity>("Common");
  const activeChar = storeSnap.selectedPlayer || playerX;
  const currencyReceipt = storeSnap.lastReceiptsByScope.currency;
  const itemReceipt = storeSnap.lastReceiptsByScope.giveItem;
  const itemOptions: readonly CatalogPickerOption[] = itemEnv.items.map((it) => ({ value: it.guid, label: it.display_name || it.name, detail: `${it.equip_slot} Lv${it.level} (${it.guid})` }));

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: "10px" }}>
      <input type="search" aria-label="Search player for currency or item grant" data-testid="give-modal-player-search-input" value={activeChar} onChange={(e) => storeSnap.setSelectedPlayer(e.currentTarget.value)} placeholder="Search player by character name, ID, or account name..." style={{ ...INPUT_STYLE, width: "100%", boxSizing: "border-box" }} />
      <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: "12px" }}>
        <section aria-label="Give Currency to Player" data-testid="give-gold-card" style={CARD_STYLE}>
          <strong style={{ display: "block", marginBottom: "8px", color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 }}>Give Gold / Currency ({activeChar || "Select Player"})</strong>
          <div style={{ display: "flex", gap: "6px", marginBottom: "8px" }}>{[1, 10, 100].map((gAdd) => <button key={gAdd} type="button" data-testid={`give-gold-chip-${gAdd}`} onClick={() => setGold((g) => g + gAdd)} style={CHIP_STYLE}>+{gAdd}g</button>)}</div>
          <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr 1fr", gap: "6px", marginBottom: "8px" }}>
            <label>Gold<input type="number" min={0} data-testid="give-gold-input" value={gold} onChange={(e) => setGold(Number(e.currentTarget.value))} style={{ ...INPUT_STYLE, width: "100%" }} /></label>
            <label>Silver<input type="number" min={0} data-testid="give-silver-input" value={silver} onChange={(e) => setSilver(Number(e.currentTarget.value))} style={{ ...INPUT_STYLE, width: "100%" }} /></label>
            <label>Copper<input type="number" min={0} data-testid="give-copper-input" value={copper} onChange={(e) => setCopper(Number(e.currentTarget.value))} style={{ ...INPUT_STYLE, width: "100%" }} /></label>
          </div>
          <div style={{ display: "flex", alignItems: "center", gap: "8px", flexWrap: "wrap" }}>
            <button type="button" data-testid="give-gold-submit" onClick={() => { try { const cmd = buildGiveMoneyCommand(activeChar, gold, silver, copper); const prose = `Granted ${gold}g ${silver}s ${copper}c to ${activeChar}`; void sendGmCommand(cmd).then((r) => { storeSnap.setScopedReceipt("currency", { ok: r.ok, message: r.ok ? prose : `${prose}: ${r.message}`, correlationId: r.correlationId }); onRecordRun({ id: `${Date.now()}-gold`, summary: prose, payload: cmd, scope: "currency" }); }); } catch (e) { storeSnap.setScopedReceipt("currency", { ok: false, message: (e as Error).message }); } }} style={BTN_STYLE}>Give Currency</button>
            {currencyReceipt ? <span role="status" data-testid="currency-receipt-banner" style={{ color: currencyReceipt.ok ? SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500 : SANDAL_LAUNCHER_TOKENS.VerraRuinCrimson500, fontSize: "11px" }}>{currencyReceipt.message}</span> : null}
          </div>
        </section>
        <section aria-label="Give Item to Player" data-testid="give-item-card" style={CARD_STYLE}>
          <strong style={{ display: "block", marginBottom: "8px", color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 }}>Give Item ({activeChar || "Select Player"})</strong>
          <CatalogComboboxPicker label="Item Definition (Catalog Search)" testIdPrefix="catalog-items" placeholder="Search items by name or GUID..." value={selectedItem} text={itemQuery} options={itemOptions} onTextChange={onItemQueryChange} onSelectOption={(opt) => { setSelectedItem(opt.value); onItemQueryChange(opt.label); }} />
          <div style={{ display: "flex", gap: "4px", marginBottom: "8px", flexWrap: "wrap" }}>{[1, 5, 20, 100].map((qtyPill) => <button key={qtyPill} type="button" data-testid={`give-item-qty-pill-${qtyPill}`} onClick={() => setCount(qtyPill)} style={{ ...CHIP_STYLE, backgroundColor: count === qtyPill ? SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: count === qtyPill ? SANDAL_LAUNCHER_TOKENS.VerraObsidian100 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100 }}>Qty {qtyPill}</button>)}</div>
          <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr auto", gap: "6px", alignItems: "end" }}>
            <label>Qty (1..100)<input type="number" min={1} max={100} data-testid="give-item-qty" value={count} onChange={(e) => setCount(Number(e.currentTarget.value))} style={{ ...INPUT_STYLE, width: "100%" }} /></label>
            <label>Rarity (1..6)<select data-testid="give-item-rarity" value={rarity} onChange={(e) => setRarity(e.currentTarget.value as ItemRarity)} style={{ ...INPUT_STYLE, width: "100%" }}>{ITEM_RARITIES.map((r, i) => <option key={r} value={r}>{i + 1} - {r}</option>)}</select></label>
            <button type="button" data-testid="give-item-submit" onClick={() => { try { const cmd = buildGrantItemCommand(activeChar, selectedItem, count, rarity); const itemLabel = itemOptions.find((o) => o.value === selectedItem)?.label || selectedItem; const prose = `Granted ${count}x ${itemLabel} (${rarity}) to ${activeChar}`; void sendGmCommand(cmd).then((r) => { storeSnap.setScopedReceipt("giveItem", { ok: r.ok, message: r.ok ? prose : `${prose}: ${r.message}`, correlationId: r.correlationId }); onRecordRun({ id: `${Date.now()}-item`, summary: prose, payload: cmd, scope: "giveItem" }); }); } catch (e) { storeSnap.setScopedReceipt("giveItem", { ok: false, message: (e as Error).message }); } }} style={BTN_STYLE}>Grant Item</button>
          </div>
          {itemReceipt ? <div role="status" data-testid="give-item-receipt-banner" style={{ marginTop: "6px", color: itemReceipt.ok ? SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500 : SANDAL_LAUNCHER_TOKENS.VerraRuinCrimson500, fontSize: "11px" }}>{itemReceipt.message}</div> : null}
        </section>
      </div>
    </div>
  );
}

interface GmModalDialogProps {
  readonly modal: Exclude<GmModalId, null>;
  readonly activePlayer: string;
  readonly playerEnv: PaginatedListEnvelope<OnlinePlayerRecord>;
  readonly itemEnv: PaginatedListEnvelope<CatalogItemEntry>;
  readonly npcEnv: PaginatedListEnvelope<CatalogNpcEntry>;
  readonly placeEnv: PaginatedListEnvelope<CatalogPlaceEntry>;
  readonly itemQuery: string;
  readonly npcQuery: string;
  readonly setItemQuery: (q: string) => void;
  readonly setNpcQuery: (q: string) => void;
  readonly onRecordRun: (entry: RecentGmRunEntry) => void;
  readonly storeSnap: WorldStreamState;
}

function GmModalDialogBody(props: GmModalDialogProps): React.ReactElement {
  const { modal, activePlayer, playerEnv, itemEnv, npcEnv, placeEnv, itemQuery, npcQuery, setItemQuery, setNpcQuery, onRecordRun, storeSnap } = props;
  return (
    <div
      role="dialog"
      aria-modal="true"
      aria-label={`GM Modal: ${modal}`}
      data-testid={`gm-modal-${modal}`}
      onKeyDown={(e) => {
        if (e.key === "Escape") storeSnap.setActiveModal(null);
      }}
      onClick={(e) => {
        if (e.target === e.currentTarget) storeSnap.setActiveModal(null);
      }}
      style={{
        position: "fixed",
        inset: 0,
        zIndex: 9999,
        display: "flex",
        alignItems: "center",
        justifyContent: "center",
        background: "rgba(6, 8, 14, 0.78)",
        backdropFilter: "blur(8px)",
        padding: "24px",
      }}
    >
      <div style={{ width: "min(640px, 92vw)", maxHeight: "86vh", overflowY: "auto", ...CARD_STYLE }}>
        <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: "10px" }}>
          <strong style={{ color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400, letterSpacing: "0.06em" }}>SANDALCORE GM ACTION</strong>
          <button type="button" data-testid="close-gm-modal-btn" onClick={() => storeSnap.setActiveModal(null)} style={CHIP_STYLE}>Close (Esc)</button>
        </div>
        {modal === "give-item" || modal === "give-currency" ? (
          <GiveGoldAndItemCards playerX={activePlayer} itemEnv={itemEnv} itemQuery={itemQuery} onItemQueryChange={setItemQuery} onRecordRun={onRecordRun} />
        ) : modal === "teleport" ? (
          <TeleportCard playerX={activePlayer} players={playerEnv.items} places={placeEnv.items} storeSnap={storeSnap} onRecordRun={onRecordRun} />
        ) : modal === "spawn-npc" ? (
          <SpawnNpcCard npcEnv={npcEnv} npcQuery={npcQuery} places={placeEnv.items} players={playerEnv.items} onNpcQueryChange={setNpcQuery} />
        ) : (
          <NodeGovernanceModalCard storeSnap={storeSnap} />
        )}
      </div>
    </div>
  );
}

function GmModalPortal(props: GmModalDialogProps): React.ReactElement {
  const bodyEl = React.createElement(GmModalDialogBody, props);
  return typeof document !== "undefined" && document.body
    ? (createPortal(bodyEl, document.body) as unknown as React.ReactElement)
    : bodyEl;
}

export function GmActionModalsDock(): React.ReactElement {
  const storeSnap = useWorldStreamStore();
  const [itemQuery, setItemQuery] = React.useState("");
  const [npcQuery, setNpcQuery] = React.useState("");
  const [, setRecentRuns] = React.useState<readonly RecentGmRunEntry[]>([]);
  const playerEnv = usePaginatedResource<OnlinePlayerRecord>("/api/v1/world/players", "players", DEFAULT_ONLINE_PLAYERS, storeSnap.selectedPlayer);
  const itemEnv = usePaginatedResource<CatalogItemEntry>("/api/v1/catalog/items", "items", DEFAULT_CATALOG_ITEMS, itemQuery);
  const npcEnv = usePaginatedResource<CatalogNpcEntry>("/api/v1/catalog/npcs", "npcs", DEFAULT_CATALOG_NPCS, npcQuery);
  const placeEnv = usePaginatedResource<CatalogPlaceEntry>("/api/v1/catalog/places", "places", DEFAULT_CATALOG_PLACES, "");
  const activePlayer = storeSnap.selectedPlayer || (playerEnv.items.length === 1 ? playerEnv.items[0].name : "");
  const modal = storeSnap.activeModal;

  React.useEffect(() => {
    if (!modal || typeof window === "undefined") return;
    const onKey = (e: KeyboardEvent): void => {
      if (e.key === "Escape") storeSnap.setActiveModal(null);
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [modal]);

  return (
    <div data-testid="gm-action-modals-dock">
      {modal ? (
        <GmModalPortal
          modal={modal}
          activePlayer={activePlayer}
          playerEnv={playerEnv}
          itemEnv={itemEnv}
          npcEnv={npcEnv}
          placeEnv={placeEnv}
          itemQuery={itemQuery}
          npcQuery={npcQuery}
          setItemQuery={setItemQuery}
          setNpcQuery={setNpcQuery}
          onRecordRun={(r) => setRecentRuns((p) => [r, ...p.slice(0, 7)])}
          storeSnap={storeSnap}
        />
      ) : null}
    </div>
  );
}

export function PlayersAndSpawnerTab(): React.ReactElement {
  const storeSnap = useWorldStreamStore();
  const [playerQuery, setPlayerQuery] = React.useState("");
  const [itemQuery, setItemQuery] = React.useState("");
  const [npcQuery, setNpcQuery] = React.useState("");
  const [recentRuns, setRecentRuns] = React.useState<readonly RecentGmRunEntry[]>([]);
  const playerEnv = usePaginatedResource<OnlinePlayerRecord>("/api/v1/world/players", "players", DEFAULT_ONLINE_PLAYERS, playerQuery);
  const itemEnv = usePaginatedResource<CatalogItemEntry>("/api/v1/catalog/items", "items", DEFAULT_CATALOG_ITEMS, itemQuery);
  const npcEnv = usePaginatedResource<CatalogNpcEntry>("/api/v1/catalog/npcs", "npcs", DEFAULT_CATALOG_NPCS, npcQuery);
  const placeEnv = usePaginatedResource<CatalogPlaceEntry>("/api/v1/catalog/places", "places", DEFAULT_CATALOG_PLACES, "");
  const activePlayer = storeSnap.selectedPlayer || (playerEnv.items.length === 1 ? playerEnv.items[0].name : "");
  const recordRun = (entry: RecentGmRunEntry): void => setRecentRuns((prev) => [entry, ...prev.slice(0, 7)]);

  return (
    <div data-testid="players-and-spawner-tab" style={{ display: "flex", flexDirection: "column", gap: "12px", padding: "12px", backgroundColor: SANDAL_LAUNCHER_TOKENS.VerraObsidian100, color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100 }}>
      <section aria-label="Online Players Roster" data-testid="players-roster-card" style={CARD_STYLE}>
        <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", gap: "8px" }}>
          <strong>Online Players (Selected Player: {activePlayer || "None"})</strong>
          <input type="search" aria-label="Search online players" data-testid="players-search-input" value={playerQuery} onChange={(e) => setPlayerQuery(e.currentTarget.value)} placeholder="Search online players..." style={{ ...INPUT_STYLE, width: "220px" }} />
        </div>
        {playerEnv.items.length === 0 ? <div role="status" data-testid="players-empty-state" style={{ paddingTop: "6px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400 }}>0 players currently online.</div> : null}
      </section>
      <GiveGoldAndItemCards playerX={activePlayer} itemEnv={itemEnv} itemQuery={itemQuery} onItemQueryChange={setItemQuery} onRecordRun={recordRun} />
      <TeleportCard playerX={activePlayer} players={playerEnv.items} places={placeEnv.items} storeSnap={storeSnap} onRecordRun={recordRun} />
      <SpawnNpcCard npcEnv={npcEnv} npcQuery={npcQuery} places={placeEnv.items} players={playerEnv.items} onNpcQueryChange={setNpcQuery} />
      <NodeGovernanceModalCard storeSnap={storeSnap} />
      {recentRuns.length > 0 ? (
        <section aria-label="Recent GM Runs Replay Strip" data-testid="recent-gm-runs-strip" style={CARD_STYLE}>
          <div style={{ display: "flex", gap: "6px", flexWrap: "wrap" }}>
            {recentRuns.map((run) => (
              <button key={run.id} type="button" onClick={() => void sendGmCommand(run.payload).then((r) => storeSnap.setScopedReceipt(run.scope, { ok: r.ok, message: r.message, correlationId: r.correlationId }))} style={CHIP_STYLE}>{run.summary}</button>
            ))}
          </div>
        </section>
      ) : null}
    </div>
  );
}

export default PlayersAndSpawnerTab;
