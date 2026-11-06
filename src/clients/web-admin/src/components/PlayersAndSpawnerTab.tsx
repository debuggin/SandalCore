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
  if (fallbackKey === "players") return true;
  return parsed.items.length > 0 || query.trim().length > 0;
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

export function TeleportCard(props: TeleportCardProps): React.ReactElement {
  const { playerX, players, places, storeSnap, onRecordRun } = props;
  const mode = storeSnap.teleportMode;
  const [targetPlayer, setTargetPlayer] = React.useState("");
  const [operatorChar, setOperatorChar] = React.useState("");
  const [placeText, setPlaceText] = React.useState("Winstead");
  const [targetPlace, setTargetPlace] = React.useState(DEFAULT_CATALOG_PLACES[2]?.name ?? "Verra_RVR_Winstead");
  const pin = storeSnap.lastClickedPoint ?? storeSnap.pickedCoordinate;
  const [coords, setCoords] = React.useState({
    x: Math.round(Number(pin?.xCm ?? -644519)),
    y: Math.round(Number(pin?.yCm ?? 376773)),
    z: Math.round(Number(pin?.zCm ?? 12012)),
  });
  const receipt = storeSnap.lastReceiptsByScope.teleport;
  const speedReceipt = storeSnap.lastReceiptsByScope.moveSpeed;
  const placeOpts: readonly CatalogPickerOption[] = places.map((pl) => ({ value: pl.name, label: pl.display_name, detail: pl.name }));

  const submitTeleport = async (): Promise<void> => {
    try {
      const matchedPlace = places.find((pl) => pl.name === targetPlace);
      const matchedPlayer = players.find((pl) => pl.name === (mode === "player" ? targetPlayer : playerX));
      const tx = mode === "place" ? (matchedPlace?.x_cm ?? coords.x) : mode === "coords" ? coords.x : (matchedPlayer?.x_cm ?? coords.x);
      const ty = mode === "place" ? (matchedPlace?.y_cm ?? coords.y) : mode === "coords" ? coords.y : (matchedPlayer?.y_cm ?? coords.y);
      const worker = resolveWorkerForCoordinateOrZone(tx, ty, matchedPlayer?.zone, undefined, mode === "place" ? targetPlace : undefined);
      const baseCmd =
        mode === "player"
          ? buildTeleportPlayerCommand({ character: playerX, target_player: targetPlayer })
          : mode === "to_player"
            ? buildTeleportPlayerCommand({ character: operatorChar || playerX, target_player: playerX })
            : mode === "place"
              ? buildTeleportPlayerCommand({ character: playerX, place: targetPlace })
              : buildTeleportPlayerCommand({ character: playerX, x_cm: coords.x, y_cm: coords.y, z_cm: coords.z });
      const cmd = { ...baseCmd, zone: worker.scope_key, worker_id: worker.worker_id };
      const res = await sendGmCommand(cmd);
      storeSnap.setScopedReceipt("teleport", { ok: res.ok, message: res.message, correlationId: res.correlationId });
      onRecordRun({ id: `${Date.now()}-tp`, summary: `Teleport ${cmd.character} (${mode})`, payload: cmd, scope: "teleport" });
    } catch (err) {
      storeSnap.setScopedReceipt("teleport", { ok: false, message: (err as Error).message });
    }
  };

  return (
    <section aria-label="Teleport Player Card" data-testid="teleport-player-card" style={CARD_STYLE}>
      <strong style={{ display: "block", marginBottom: "8px", color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 }}>
        Teleport & Movement Speed ({playerX || "Select Player X"})
      </strong>
      <div style={{ display: "flex", gap: "6px", marginBottom: "8px", flexWrap: "wrap" }}>
        {(
          [
            { id: "place", label: "Player X -> Named Place" },
            { id: "player", label: "Player X -> Player Y" },
            { id: "to_player", label: "Teleport to Player X" },
            { id: "coords", label: "Player X -> Map Location (X, Y, Z)" },
          ] as const
        ).map((m) => (
          <button key={m.id} type="button" data-testid={`teleport-mode-${m.id}`} onClick={() => storeSnap.setTeleportMode(m.id)} style={{ ...CHIP_STYLE, backgroundColor: mode === m.id ? SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: mode === m.id ? SANDAL_LAUNCHER_TOKENS.VerraObsidian100 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100 }}>
            {m.label}
          </button>
        ))}
      </div>
      {mode === "place" ? (
        <CatalogComboboxPicker label="Destination Named Place" testIdPrefix="teleport-place" placeholder="Type place name..." value={targetPlace} text={placeText} options={placeOpts} presets={placeOpts.slice(0, 4)} onTextChange={setPlaceText} onSelectOption={(o) => { setTargetPlace(o.value); setPlaceText(o.label); }} />
      ) : mode === "player" ? (
        <input type="text" aria-label="Target Player Y" data-testid="teleport-target-player" value={targetPlayer} onChange={(e) => setTargetPlayer(e.currentTarget.value)} placeholder="Destination Player Y name or GUID..." style={{ ...INPUT_STYLE, width: "100%", marginBottom: "8px" }} />
      ) : mode === "to_player" ? (
        <input type="text" aria-label="Operator Character" data-testid="teleport-operator-char" value={operatorChar} onChange={(e) => setOperatorChar(e.currentTarget.value)} placeholder={`Operator character to move to ${playerX || "Player X"}...`} style={{ ...INPUT_STYLE, width: "100%", marginBottom: "8px" }} />
      ) : (
        <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr 1fr auto", gap: "4px", marginBottom: "8px" }}>
          <input type="number" aria-label="Teleport X cm" data-testid="teleport-x" value={coords.x} onChange={(e) => setCoords({ ...coords, x: Number(e.currentTarget.value) })} style={INPUT_STYLE} />
          <input type="number" aria-label="Teleport Y cm" data-testid="teleport-y" value={coords.y} onChange={(e) => setCoords({ ...coords, y: Number(e.currentTarget.value) })} style={INPUT_STYLE} />
          <input type="number" aria-label="Teleport Z cm" data-testid="teleport-z" value={coords.z} onChange={(e) => setCoords({ ...coords, z: Number(e.currentTarget.value) })} style={INPUT_STYLE} />
          <button type="button" data-testid="teleport-use-picked" onClick={() => pin && setCoords({ x: Math.round(Number(pin.xCm ?? 0)), y: Math.round(Number(pin.yCm ?? 0)), z: Math.round(Number(pin.zCm ?? 0)) })} style={CHIP_STYLE}>Use Map Pin</button>
        </div>
      )}
      <div style={{ display: "flex", alignItems: "center", gap: "8px", flexWrap: "wrap", marginBottom: "8px" }}>
        <button type="button" data-testid="teleport-submit-btn" onClick={() => void submitTeleport()} style={BTN_STYLE}>Teleport Player</button>
        {players.length > 0 && mode === "player" ? <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, fontSize: "11px" }}>Online: {players.map((p) => p.name).join(", ")}</span> : null}
        {receipt ? <span role="status" data-testid="teleport-receipt-banner" style={{ color: receipt.ok ? SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500 : SANDAL_LAUNCHER_TOKENS.VerraRuinCrimson500, fontSize: "11px" }}>{receipt.message}</span> : null}
      </div>
      <div style={{ display: "flex", alignItems: "center", gap: "6px", flexWrap: "wrap", borderTop: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, paddingTop: "8px" }}>
        <span style={{ color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, fontSize: "11px" }}>Move Speed:</span>
        {MOVE_SPEED_MULTIPLIERS.map((mult) => (
          <button key={mult} type="button" data-testid={`move-speed-${mult}x`} onClick={() => void dispatchAuxiliaryGmAction({ action: "set_move_speed", character: playerX, speed_multiplier: mult }).then((r) => storeSnap.setScopedReceipt("moveSpeed", { ok: r.ok, message: `${playerX || "Player"} speed -> ${mult}x: ${r.message}` }))} style={CHIP_STYLE}>
            {mult}x
          </button>
        ))}
        {speedReceipt ? <span data-testid="move-speed-receipt-banner" style={{ color: SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500, fontSize: "11px" }}>{speedReceipt.message}</span> : null}
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
  const currencyReceipt = storeSnap.lastReceiptsByScope.currency;
  const itemReceipt = storeSnap.lastReceiptsByScope.giveItem;
  const itemOptions: readonly CatalogPickerOption[] = itemEnv.items.map((it) => ({
    value: it.guid,
    label: it.display_name || it.name,
    detail: `${it.equip_slot} Lv${it.level} (${it.guid})`,
  }));
  const starterItemPresets: readonly CatalogPickerOption[] = DEFAULT_CATALOG_ITEMS.map((it) => ({
    value: it.guid,
    label: it.display_name,
    detail: it.guid,
  }));

  return (
    <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: "12px" }}>
      <section aria-label="Give Currency to Player X" data-testid="give-gold-card" style={CARD_STYLE}>
        <strong style={{ display: "block", marginBottom: "8px", color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 }}>
          Give Gold / Currency ({playerX || "Select Player X"})
        </strong>
        <div style={{ display: "flex", gap: "6px", marginBottom: "8px" }}>
          {[1, 10, 100].map((gAdd) => (
            <button key={gAdd} type="button" data-testid={`give-gold-chip-${gAdd}`} onClick={() => setGold((g) => g + gAdd)} style={CHIP_STYLE}>+{gAdd}g</button>
          ))}
        </div>
        <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr 1fr", gap: "6px", marginBottom: "8px" }}>
          <label>Gold<input type="number" min={0} data-testid="give-gold-input" value={gold} onChange={(e) => setGold(Number(e.currentTarget.value))} style={{ ...INPUT_STYLE, width: "100%" }} /></label>
          <label>Silver<input type="number" min={0} data-testid="give-silver-input" value={silver} onChange={(e) => setSilver(Number(e.currentTarget.value))} style={{ ...INPUT_STYLE, width: "100%" }} /></label>
          <label>Copper<input type="number" min={0} data-testid="give-copper-input" value={copper} onChange={(e) => setCopper(Number(e.currentTarget.value))} style={{ ...INPUT_STYLE, width: "100%" }} /></label>
        </div>
        <div style={{ display: "flex", alignItems: "center", gap: "8px", flexWrap: "wrap" }}>
          <button type="button" data-testid="give-gold-submit" onClick={() => {
            try {
              const cmd = buildGiveMoneyCommand(playerX, gold, silver, copper);
              void sendGmCommand(cmd).then((r) => {
                storeSnap.setScopedReceipt("currency", { ok: r.ok, message: r.message, correlationId: r.correlationId });
                onRecordRun({ id: `${Date.now()}-gold`, summary: `Give ${gold}g ${silver}s ${copper}c to ${playerX}`, payload: cmd, scope: "currency" });
              });
            } catch (e) {
              storeSnap.setScopedReceipt("currency", { ok: false, message: (e as Error).message });
            }
          }} style={BTN_STYLE}>Give Currency</button>
          {currencyReceipt ? <span role="status" data-testid="currency-receipt-banner" style={{ color: currencyReceipt.ok ? SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500 : SANDAL_LAUNCHER_TOKENS.VerraRuinCrimson500, fontSize: "11px" }}>{currencyReceipt.message}</span> : null}
        </div>
      </section>

      <section aria-label="Give Item to Player X" data-testid="give-item-card" style={CARD_STYLE}>
        <strong style={{ display: "block", marginBottom: "8px", color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 }}>
          Give Item ({playerX || "Select Player X"})
        </strong>
        <CatalogComboboxPicker label="Item Definition (Catalog Search)" testIdPrefix="catalog-items" placeholder="Search items by name or GUID..." value={selectedItem} text={itemQuery} options={itemOptions} presets={starterItemPresets} onTextChange={onItemQueryChange} onSelectOption={(opt) => { setSelectedItem(opt.value); onItemQueryChange(opt.label); }} />
        <div style={{ display: "flex", gap: "4px", marginBottom: "8px", flexWrap: "wrap" }}>
          {[1, 5, 20, 100].map((qtyPill) => (
            <button key={qtyPill} type="button" data-testid={`give-item-qty-pill-${qtyPill}`} onClick={() => setCount(qtyPill)} style={{ ...CHIP_STYLE, backgroundColor: count === qtyPill ? SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: count === qtyPill ? SANDAL_LAUNCHER_TOKENS.VerraObsidian100 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100 }}>Qty {qtyPill}</button>
          ))}
        </div>
        <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr auto", gap: "6px", alignItems: "end" }}>
          <label>Qty (1..100)<input type="number" min={1} max={100} data-testid="give-item-qty" value={count} onChange={(e) => setCount(Number(e.currentTarget.value))} style={{ ...INPUT_STYLE, width: "100%" }} /></label>
          <label>Rarity (1..6)<select data-testid="give-item-rarity" value={rarity} onChange={(e) => setRarity(e.currentTarget.value as ItemRarity)} style={{ ...INPUT_STYLE, width: "100%" }}>{ITEM_RARITIES.map((r, i) => <option key={r} value={r}>{i + 1} - {r}</option>)}</select></label>
          <button type="button" data-testid="give-item-submit" onClick={() => {
            try {
              const cmd = buildGrantItemCommand(playerX, selectedItem, count, rarity);
              void sendGmCommand(cmd).then((r) => {
                storeSnap.setScopedReceipt("giveItem", { ok: r.ok, message: r.message, correlationId: r.correlationId });
                onRecordRun({ id: `${Date.now()}-item`, summary: `Grant ${count}x ${selectedItem} (${rarity}) to ${playerX}`, payload: cmd, scope: "giveItem" });
              });
            } catch (e) {
              storeSnap.setScopedReceipt("giveItem", { ok: false, message: (e as Error).message });
            }
          }} style={BTN_STYLE}>Grant Item</button>
        </div>
        {itemReceipt ? <div role="status" data-testid="give-item-receipt-banner" style={{ marginTop: "6px", color: itemReceipt.ok ? SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500 : SANDAL_LAUNCHER_TOKENS.VerraRuinCrimson500, fontSize: "11px" }}>{itemReceipt.message}</div> : null}
      </section>
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
  const [playerQuery, setPlayerQuery] = React.useState("");
  const [itemQuery, setItemQuery] = React.useState("");
  const [npcQuery, setNpcQuery] = React.useState("");
  const [, setRecentRuns] = React.useState<readonly RecentGmRunEntry[]>([]);
  const playerEnv = usePaginatedResource<OnlinePlayerRecord>("/api/v1/world/players", "players", DEFAULT_ONLINE_PLAYERS, playerQuery);
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

  const modals: readonly { readonly id: Exclude<GmModalId, null>; readonly label: string }[] = [
    { id: "give-item", label: "Give Item" },
    { id: "give-currency", label: "Give Gold" },
    { id: "teleport", label: "Teleport" },
    { id: "spawn-npc", label: "Spawn NPC" },
    { id: "node-governance", label: "Node Governance" },
  ];

  return (
    <div data-testid="gm-action-modals-dock" style={{ display: "flex", alignItems: "center", gap: "6px", flexWrap: "wrap" }}>
      <input
        type="search"
        aria-label="Search online players"
        data-testid="players-search-input"
        value={playerQuery || storeSnap.selectedPlayer}
        onChange={(e) => {
          setPlayerQuery(e.currentTarget.value);
          storeSnap.setSelectedPlayer(e.currentTarget.value);
        }}
        placeholder="Player X search..."
        style={{ ...INPUT_STYLE, width: "170px" }}
      />
      {modals.map((m) => (
        <button key={m.id} type="button" data-testid={`open-modal-${m.id}`} onClick={() => storeSnap.setActiveModal(modal === m.id ? null : m.id)} style={{ ...CHIP_STYLE, backgroundColor: modal === m.id ? SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: modal === m.id ? SANDAL_LAUNCHER_TOKENS.VerraObsidian100 : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100 }}>
          {m.label}
        </button>
      ))}
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
          <strong>Online Players (Active Player X: {activePlayer || "None"})</strong>
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
