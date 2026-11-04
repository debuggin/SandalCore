// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

import React from "react";
import { formatDeterministicUuidV7, type GmRole } from "./GmCommandPalette.tsx";

export type HotfixViewState = "loading" | "empty" | "error" | "degraded" | "populated";

export const ALLOWED_HOTFIX_TABLES: readonly string[] = [
  "world_hotfix_overlays", "spawns", "loot_tables", "abilities", "items", "npcs",
];

const FORBIDDEN_DDL_TOKENS: readonly string[] = [
  "DROP", "TRUNCATE", "ALTER", "ATTACH", "DETACH", "PRAGMA", "VACUUM",
];

export interface HotfixSqlValidationResult {
  readonly valid: boolean;
  readonly statementType?: "INSERT" | "UPDATE" | "DELETE";
  readonly targetTable?: string;
  readonly errorCode?: string;
  readonly errorMessage?: string;
}

export interface HotfixRowDiff {
  readonly table: string;
  readonly primaryKey: string;
  readonly column: string;
  readonly oldValue: string;
  readonly newValue: string;
}

export interface HotfixDryRunRequest {
  readonly sql: string;
  readonly operatorRole: GmRole | string;
  readonly snapshotEpoch?: number;
  readonly existingValue?: string;
  readonly sequence?: number;
}

export interface HotfixDryRunPreview {
  readonly valid: boolean;
  readonly viewState: HotfixViewState;
  readonly statementType: string;
  readonly targetTable: string;
  readonly explainPlan: readonly string[];
  readonly estimatedRows: number;
  readonly rowDiffs: readonly HotfixRowDiff[];
  readonly generatedUndoSql: string;
  readonly canApply: boolean;
  readonly idempotencyKey: string;
  readonly snapshotEpoch: number;
  readonly errorMessage?: string;
}

export interface HotfixHttpPayload {
  readonly sql: string;
  readonly reason?: string;
  readonly actorSub?: string;
  readonly expectedUndoSql?: string;
  readonly idempotencyKey?: string;
  readonly sequence?: number;
}

export interface HotfixHttpRequestEnvelope {
  readonly method: "POST";
  readonly url: "/api/v1/gm/hotfix/dry-run" | "/api/v1/gm/hotfix/apply";
  readonly headers: Readonly<Record<string, string>>;
  readonly body: string;
}

export interface HotfixEditorProps {
  readonly sql: string;
  readonly operatorRole: GmRole;
  readonly preview: HotfixDryRunPreview | null;
  readonly isLoading?: boolean;
  readonly isDegraded?: boolean;
  readonly errorBanner?: string | null;
  readonly onSqlChange: (nextSql: string) => void;
  readonly onDryRun: () => void;
  readonly onApply: () => void;
}

export function validateClientHotfixSql(sql: string): HotfixSqlValidationResult {
  const trimmed = sql.trim().replace(/;+$/, "").trim();
  if (trimmed.length === 0) {
    return { valid: false, errorCode: "empty-sql", errorMessage: "Hotfix SQL statement must not be empty." };
  }
  if (trimmed.includes(";")) {
    return { valid: false, errorCode: "multi-statement", errorMessage: "Multi-statement SQL batches are forbidden." };
  }
  const tokens = trimmed.toUpperCase().replace(/[^A-Z0-9_]/g, " ").split(/\s+/).filter(Boolean);
  for (const token of tokens) {
    if (FORBIDDEN_DDL_TOKENS.includes(token)) {
      return { valid: false, errorCode: "forbidden-ddl", errorMessage: `Forbidden DDL or administrative token: ${token}` };
    }
  }
  return extractHotfixTarget(tokens);
}

function extractHotfixTarget(tokens: readonly string[]): HotfixSqlValidationResult {
  const verb = tokens[0];
  let tableCandidate = "";
  if (verb === "INSERT" && tokens[1] === "INTO" && tokens[2]) tableCandidate = tokens[2].toLowerCase();
  else if (verb === "UPDATE" && tokens[1]) tableCandidate = tokens[1].toLowerCase();
  else if (verb === "DELETE" && tokens[1] === "FROM" && tokens[2]) tableCandidate = tokens[2].toLowerCase();
  else {
    return { valid: false, errorCode: "unsupported-verb", errorMessage: "Hotfix SQL must start with INSERT INTO, UPDATE, or DELETE FROM." };
  }
  if (!ALLOWED_HOTFIX_TABLES.includes(tableCandidate)) {
    return { valid: false, errorCode: "table-not-allowed", errorMessage: `Target table '${tableCandidate}' is outside the hotfix overlay allowlist.` };
  }
  return { valid: true, statementType: verb as "INSERT" | "UPDATE" | "DELETE", targetTable: tableCandidate };
}

function parseSqlOverlayMutation(sql: string): { primaryKey: string; column: string; newValue: string } {
  const quoted = Array.from(sql.matchAll(/'([^']*)'/g), (m) => m[1]);
  const setMatch = sql.match(/\bSET\s+([a-zA-Z0-9_]+)\s*=/i);
  const column = setMatch ? setMatch[1] : "payload_json";
  const upper = sql.trim().toUpperCase();
  if (upper.startsWith("INSERT")) {
    return { primaryKey: quoted[0] ?? "overlay:default", column, newValue: quoted[1] ?? "{\"patched\":true}" };
  }
  if (upper.startsWith("DELETE")) {
    return { primaryKey: quoted[0] ?? "overlay:default", column, newValue: "DELETED" };
  }
  return { primaryKey: quoted[1] ?? quoted[0] ?? "overlay:default", column, newValue: quoted[0] ?? "{\"patched\":true}" };
}

function synthesizeUndoSql(
  stmtType: "INSERT" | "UPDATE" | "DELETE",
  table: string,
  primaryKey: string,
  column: string,
  oldValue: string
): string {
  const safeKey = primaryKey.replace(/'/g, "''");
  const safeOld = oldValue.replace(/'/g, "''");
  if (stmtType === "INSERT" && oldValue === "null") {
    return `DELETE FROM ${table} WHERE overlay_key = '${safeKey}';`;
  }
  if (stmtType === "DELETE") {
    return `INSERT INTO ${table} (overlay_key, ${column}) VALUES ('${safeKey}', '${safeOld}');`;
  }
  return `UPDATE ${table} SET ${column} = '${safeOld}' WHERE overlay_key = '${safeKey}';`;
}

export function buildHotfixDryRunPreview(request: HotfixDryRunRequest): HotfixDryRunPreview {
  const seq = request.sequence ?? 1;
  const epoch = request.snapshotEpoch ?? 1;
  const idempotencyKey = formatDeterministicUuidV7(1772800000000 + epoch * 1000, seq);
  const validation = validateClientHotfixSql(request.sql);
  if (!validation.valid || !validation.statementType || !validation.targetTable) {
    return {
      valid: false, viewState: "error", statementType: validation.statementType ?? "INVALID",
      targetTable: validation.targetTable ?? "", explainPlan: [], estimatedRows: 0, rowDiffs: [],
      generatedUndoSql: "", canApply: false, idempotencyKey, snapshotEpoch: epoch,
      errorMessage: validation.errorMessage ?? "Invalid hotfix SQL.",
    };
  }
  const parsed = parseSqlOverlayMutation(request.sql);
  const oldValue = request.existingValue ?? "null";
  const generatedUndoSql = synthesizeUndoSql(validation.statementType, validation.targetTable, parsed.primaryKey, parsed.column, oldValue);
  const rowDiff: HotfixRowDiff = {
    table: validation.targetTable, primaryKey: parsed.primaryKey, column: parsed.column, oldValue, newValue: parsed.newValue,
  };
  return {
    valid: true, viewState: "populated", statementType: validation.statementType, targetTable: validation.targetTable,
    explainPlan: [
      "BEGIN IMMEDIATE TRANSACTION /* read-only dry-run snapshot */",
      `SEARCH ${validation.targetTable} USING PRIMARY KEY (overlay_key=?)`,
      `${validation.statementType} ${validation.targetTable}.${parsed.column} (${oldValue} -> ${parsed.newValue})`,
      "ROLLBACK TRANSACTION /* zero live state mutation */",
    ],
    estimatedRows: 1, rowDiffs: [rowDiff], generatedUndoSql,
    canApply: request.operatorRole === "GM_ARCHITECT" && generatedUndoSql.trim().length > 0,
    idempotencyKey, snapshotEpoch: epoch,
  };
}

export function buildHotfixHttpRequest(mode: "dry-run" | "apply", payload: HotfixHttpPayload): HotfixHttpRequestEnvelope {
  const idemKey = payload.idempotencyKey ?? formatDeterministicUuidV7(1772800050000, payload.sequence ?? 1);
  return {
    method: "POST",
    url: mode === "dry-run" ? "/api/v1/gm/hotfix/dry-run" : "/api/v1/gm/hotfix/apply",
    headers: { "Content-Type": "application/json", "Idempotency-Key": idemKey, "X-Idempotency-Key": idemKey },
    body: JSON.stringify({
      sql: payload.sql.trim(),
      reason: payload.reason ?? "gm_studio_hotfix_overlay",
      actor_sub: payload.actorSub ?? "gm.architect",
      expected_undo_sql: payload.expectedUndoSql ?? "",
    }),
  };
}

export function resolveHotfixViewState(props: HotfixEditorProps): HotfixViewState {
  if (props.isLoading) return "loading";
  if (props.errorBanner || (props.preview && !props.preview.valid)) return "error";
  if (props.isDegraded) return "degraded";
  if (!props.preview || props.preview.rowDiffs.length === 0) return "empty";
  return "populated";
}

export function HotfixEditor(props: HotfixEditorProps): React.ReactElement {
  const viewState = resolveHotfixViewState(props);
  const canApply = Boolean(props.preview?.canApply && props.operatorRole === "GM_ARCHITECT");
  const handleKeyDown = (event: React.KeyboardEvent<HTMLElement>): void => {
    if ((event.ctrlKey || event.metaKey) && event.key === "Enter") {
      event.preventDefault();
      if ( event.shiftKey && canApply ) props.onApply();
      else props.onDryRun();
    }
  };
  return (
    <section
      role="region"
      aria-label="Hotfix SQL Overlay Editor"
      data-view-state={viewState}
      onKeyDown={handleKeyDown}
      style={{ fontVariantNumeric: "tabular-nums" }}
    >
      <header>
        <h2>Hotfix SQL Overlay Editor</h2>
        <span aria-live="polite">{props.errorBanner ?? `State: ${viewState}`}</span>
      </header>
      <textarea
        aria-label="Hotfix SQL Statement"
        value={props.sql}
        onChange={(e) => props.onSqlChange(e.target.value)}
        rows={4}
      />
      <div>
        <button type="button" onClick={props.onDryRun}>
          Dry-Run EXPLAIN &amp; Diff
        </button>
        <button type="button" disabled={!canApply} aria-disabled={!canApply} onClick={props.onApply}>
          Apply Hotfix Overlay
        </button>
      </div>
      {props.preview && (
        <div role="table" aria-label="Hotfix Row Diff Preview">
          {props.preview.rowDiffs.map((diff) => (
            <div key={`${diff.table}:${diff.primaryKey}:${diff.column}`} role="row">
              <span role="cell">{diff.table}</span>
              <span role="cell">{diff.primaryKey}</span>
              <span role="cell">{diff.column}</span>
              <span role="cell">{diff.oldValue}</span>
              <span role="cell">{diff.newValue}</span>
            </div>
          ))}
          <pre aria-label="Generated Undo SQL">{props.preview.generatedUndoSql}</pre>
        </div>
      )}
    </section>
  );
}
