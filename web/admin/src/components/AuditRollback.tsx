// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

import React from "react";
import { formatDeterministicUuidV7, type GmRole } from "./GmCommandPalette.tsx";

export type AuditViewState = "loading" | "empty" | "error" | "degraded" | "populated";

export interface AuditLogEntry {
  readonly auditId: string;
  readonly actorSub: string;
  readonly actorRole: string;
  readonly commandDomain: string;
  readonly actionName: string;
  readonly targetEntityId: string;
  readonly undoSql: string;
  readonly rolledBack: boolean;
  readonly rolledBackByAuditId?: string;
  readonly timestampIso: string;
  readonly snapshotEpoch: number;
}

export interface AuditRollbackRequestEnvelope {
  readonly method: "POST";
  readonly url: "/api/v1/gm/audit/rollback";
  readonly headers: Readonly<Record<string, string>>;
  readonly body: string;
}

export interface AuditRollbackStateInput {
  readonly entries: readonly AuditLogEntry[];
  readonly isLoading?: boolean;
  readonly isDegraded?: boolean;
  readonly errorMessage?: string | null;
}

export interface AuditRollbackProps {
  readonly entries: readonly AuditLogEntry[];
  readonly operatorRole: GmRole;
  readonly isLoading?: boolean;
  readonly isDegraded?: boolean;
  readonly errorMessage?: string | null;
  readonly onRollbackEntry: (entry: AuditLogEntry) => void;
}

export function resolveAuditViewState(input: AuditRollbackStateInput): AuditViewState {
  if (input.isLoading) {
    return "loading";
  }
  if (input.errorMessage && input.errorMessage.trim().length > 0) {
    return "error";
  }
  if (input.isDegraded) {
    return "degraded";
  }
  if (input.entries.length === 0) {
    return "empty";
  }
  return "populated";
}

export function canRollbackAuditEntry(entry: AuditLogEntry, operatorRole: GmRole | string): boolean {
  if (operatorRole !== "GM_ARCHITECT") {
    return false;
  }
  if (entry.rolledBack) {
    return false;
  }
  return entry.undoSql.trim().length > 0;
}

export function buildAuditRollbackRequest(
  entry: AuditLogEntry,
  operatorRole: GmRole | string,
  idempotencyKey?: string
): AuditRollbackRequestEnvelope {
  if (!canRollbackAuditEntry(entry, operatorRole)) {
    throw new Error(
      `Audit entry '${entry.auditId}' is not eligible for rollback by role '${operatorRole}'.`
    );
  }
  const idemKey =
    idempotencyKey && idempotencyKey.trim().length > 0
      ? idempotencyKey.trim()
      : formatDeterministicUuidV7(1772800100000 + entry.snapshotEpoch * 1000, 1);
  return {
    method: "POST",
    url: "/api/v1/gm/audit/rollback",
    headers: {
      "Content-Type": "application/json",
      "Idempotency-Key": idemKey,
      "X-Idempotency-Key": idemKey,
    },
    body: JSON.stringify({
      audit_id: entry.auditId,
      expected_undo_sql: entry.undoSql,
      actor_sub: "gm.architect",
      reason: `rollback:${entry.auditId}`,
    }),
  };
}

export function applyOptimisticAuditRollback(
  entries: readonly AuditLogEntry[],
  targetAuditId: string,
  rollbackAuditId: string,
  nextEpoch: number
): AuditLogEntry[] {
  const target = entries.find((item) => item.auditId === targetAuditId);
  if (!target) {
    throw new Error(`Target audit entry '${targetAuditId}' not found.`);
  }
  if (target.rolledBack) {
    throw new Error(`Target audit entry '${targetAuditId}' was already rolled back.`);
  }
  const rollbackRecord: AuditLogEntry = {
    auditId: rollbackAuditId,
    actorSub: "gm.architect",
    actorRole: "GM_ARCHITECT",
    commandDomain: "HOTFIX_SQL",
    actionName: "AUDIT_ROLLBACK",
    targetEntityId: target.targetEntityId,
    undoSql: "",
    rolledBack: false,
    timestampIso: target.timestampIso,
    snapshotEpoch: nextEpoch,
  };
  const updated = entries.map((item): AuditLogEntry => {
    if (item.auditId !== targetAuditId) {
      return item;
    }
    return {
      ...item,
      rolledBack: true,
      rolledBackByAuditId: rollbackAuditId,
    };
  });
  return [rollbackRecord, ...updated];
}

export function AuditRollback(props: AuditRollbackProps): React.ReactElement {
  const viewState = resolveAuditViewState({
    entries: props.entries,
    isLoading: props.isLoading,
    isDegraded: props.isDegraded,
    errorMessage: props.errorMessage,
  });
  return (
    <section
      role="region"
      aria-label="GM Audit Log & Rollback"
      data-view-state={viewState}
      style={{ fontVariantNumeric: "tabular-nums" }}
    >
      <header>
        <h2>GM Audit Log &amp; 1-Click Rollback</h2>
        <span aria-live="polite">{props.errorMessage ?? `State: ${viewState}`}</span>
      </header>
      <div role="grid" aria-label="Immutable GM Audit Entries">
        {props.entries.map((entry) => {
          const eligible = canRollbackAuditEntry(entry, props.operatorRole);
          return (
            <div key={entry.auditId} role="row" data-rolled-back={entry.rolledBack}>
              <span role="gridcell">{entry.auditId}</span>
              <span role="gridcell">{entry.actorSub}</span>
              <span role="gridcell">{entry.actionName}</span>
              <span role="gridcell">{entry.snapshotEpoch}</span>
              <code role="gridcell">{entry.undoSql}</code>
              <span role="gridcell">
                <button
                  type="button"
                  disabled={!eligible}
                  aria-disabled={!eligible}
                  onClick={() => {
                    if (eligible) {
                      props.onRollbackEntry(entry);
                    }
                  }}
                >
                  {entry.rolledBack ? "Rolled Back" : "1-Click Rollback"}
                </button>
              </span>
            </div>
          );
        })}
      </div>
    </section>
  );
}
