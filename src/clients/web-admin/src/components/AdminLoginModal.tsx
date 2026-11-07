// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import {
  clearAdminSession,
  loginWithPassphrase,
  setAdminAuthState,
  useAdminAuthStore,
} from "../state/adminAuthClient";
import { useWorldStreamStore } from "../state/worldStreamStore";
import { SANDAL_LAUNCHER_TOKENS } from "./PlayersAndSpawnerTab";

export const CANONICAL_ADMIN_JWT_SCOPES: readonly string[] = Object.freeze([
  "admin:read",
  "gm:commands",
  "gm:spawn",
  "world:read",
  "gm:write",
  "node:write",
  "logs:read",
  "containers:control",
]);

export interface AdminLoginModalProps {
  readonly isOpen?: boolean;
  readonly onClose?: () => void;
}

const MODAL_BACKDROP_STYLE: React.CSSProperties = {
  position: "fixed",
  inset: 0,
  zIndex: 120,
  display: "flex",
  alignItems: "center",
  justifyContent: "center",
  padding: "16px",
  backgroundColor: "rgba(6, 8, 14, 0.82)",
  backdropFilter: "blur(12px)",
};

const MODAL_CARD_STYLE: React.CSSProperties = {
  width: "460px",
  maxWidth: "94vw",
  padding: "20px",
  borderRadius: "12px",
  border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
  background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush,
  color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100,
  boxShadow: "0 18px 42px rgba(0, 0, 0, 0.65)",
};

const INPUT_FIELD_STYLE: React.CSSProperties = {
  width: "100%",
  minHeight: "36px",
  padding: "6px 10px",
  borderRadius: "6px",
  border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
  backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCInputFillBrush,
  color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100,
  fontSize: "13px",
  boxSizing: "border-box",
};

export function AdminAuthSessionBadge(): React.ReactElement {
  const authSnap = useAdminAuthStore();
  const isAuthenticated = Boolean(authSnap.token && authSnap.token.length > 0);
  const onActionClick = (): void => {
    if (isAuthenticated) {
      useWorldStreamStore.getState().saveSessionUiState();
      clearAdminSession();
      useWorldStreamStore.getState().resetForUnauthenticatedSession();
      setAdminAuthState({ loginPromptOpen: true, authRequired: true, lastAuthError: null });
      return;
    }
    setAdminAuthState({ loginPromptOpen: true, lastAuthError: null });
  };

  return (
    <div
      data-testid="admin-auth-session-badge"
      style={{ display: "flex", alignItems: "center", gap: "8px", flexWrap: "wrap" }}
    >
      {isAuthenticated ? (
        <span
          data-testid="admin-auth-session-identity"
          style={{
            padding: "4px 10px",
            borderRadius: "999px",
            fontSize: "11px",
            fontWeight: 700,
            border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
            backgroundColor: "rgba(32, 216, 206, 0.14)",
            color: SANDAL_LAUNCHER_TOKENS.VerraSanctumEmerald500,
          }}
        >
          {authSnap.username || "admin"}
        </span>
      ) : null}
      <button
        type="button"
        data-testid="admin-auth-session-btn"
        onClick={onActionClick}
        style={{
          minHeight: "32px",
          padding: "4px 12px",
          borderRadius: "8px",
          border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
          backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush,
          color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100,
          fontSize: "11px",
          fontWeight: 700,
          letterSpacing: "0.04em",
          cursor: "pointer",
        }}
      >
        {isAuthenticated ? "End Session" : "Start Session"}
      </button>
    </div>
  );
}

interface AdminLoginFormBodyProps {
  readonly username: string;
  readonly passphrase: string;
  readonly isSubmitting: boolean;
  readonly authRequired: boolean;
  readonly lastAuthError: string | null;
  readonly onUsernameChange: (v: string) => void;
  readonly onPassphraseChange: (v: string) => void;
  readonly onDismiss: () => void;
  readonly onSubmit: (e: React.FormEvent) => void;
}

function renderAdminLoginFormHeader(authRequired: boolean, onDismiss: () => void): React.ReactElement {
  return (
    <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: "10px" }}>
      <strong style={{ fontSize: "14px", letterSpacing: "0.08em", textTransform: "uppercase", color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 }}>
        World Admin Authentication
      </strong>
      {!authRequired ? (
        <button
          type="button"
          data-testid="admin-login-dismiss"
          onClick={onDismiss}
          style={{ minHeight: "28px", padding: "2px 8px", borderRadius: "6px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush, color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, fontSize: "11px", cursor: "pointer" }}
        >
          Cancel
        </button>
      ) : null}
    </div>
  );
}

function renderAdminLoginFormBody(p: AdminLoginFormBodyProps): React.ReactElement {
  return (
    <form onSubmit={p.onSubmit} style={MODAL_CARD_STYLE}>
      {renderAdminLoginFormHeader(p.authRequired, p.onDismiss)}
      <p style={{ margin: "0 0 12px 0", fontSize: "12px", color: SANDAL_LAUNCHER_TOKENS.VerraMutedAsh400, lineHeight: 1.45 }}>
        Enter the ephemeral Admin passphrase printed in the server startup console.
      </p>
      {p.lastAuthError ? (
        <div role="alert" data-testid="admin-login-error" style={{ marginBottom: "12px", padding: "8px 10px", borderRadius: "6px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.VerraRuinCrimson500}`, backgroundColor: "rgba(224, 82, 88, 0.16)", color: SANDAL_LAUNCHER_TOKENS.VerraRuinCrimson500, fontSize: "12px" }}>
          {p.lastAuthError}
        </div>
      ) : null}
      <label style={{ display: "block", marginBottom: "10px", fontSize: "12px", fontWeight: 600 }}>
        Operator Username
        <input type="text" aria-label="Operator Username" data-testid="admin-login-username" value={p.username} onChange={(e) => p.onUsernameChange(e.currentTarget.value)} autoComplete="username" style={{ ...INPUT_FIELD_STYLE, marginTop: "4px" }} />
      </label>
      <label style={{ display: "block", marginBottom: "14px", fontSize: "12px", fontWeight: 600 }}>
        Startup Console Passphrase
        <input type="password" aria-label="Startup Console Passphrase" data-testid="admin-login-passphrase" value={p.passphrase} onChange={(e) => p.onPassphraseChange(e.currentTarget.value)} placeholder="Paste Passphrase from startup console banner..." autoComplete="current-password" style={{ ...INPUT_FIELD_STYLE, marginTop: "4px" }} />
      </label>
      <button type="submit" data-testid="admin-login-submit" disabled={p.isSubmitting} style={{ width: "100%", minHeight: "38px", padding: "8px 14px", borderRadius: "8px", border: `1px solid ${SANDAL_LAUNCHER_TOKENS.VerraRelicGold500}`, backgroundColor: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500, color: SANDAL_LAUNCHER_TOKENS.VerraObsidian100, fontSize: "12px", fontWeight: 700, letterSpacing: "0.06em", textTransform: "uppercase", cursor: p.isSubmitting ? "wait" : "pointer" }}>
        {p.isSubmitting ? "Starting Session..." : "Start Session"}
      </button>
    </form>
  );
}

export function AdminLoginModal(props?: AdminLoginModalProps): React.ReactElement | null {
  const authSnap = useAdminAuthStore();
  const isModalOpen = props?.isOpen ?? (authSnap.loginPromptOpen || authSnap.authRequired);
  const [username, setUsername] = React.useState(authSnap.username || "admin");
  const [passphrase, setPassphrase] = React.useState("");
  const [isSubmitting, setIsSubmitting] = React.useState(false);

  React.useEffect(() => {
    if (authSnap.username && authSnap.username.trim().length > 0) {
      setUsername(authSnap.username);
    }
  }, [authSnap.username]);

  if (!isModalOpen) return null;

  const handleSubmit = async (e: React.FormEvent): Promise<void> => {
    e.preventDefault();
    if (isSubmitting) return;
    setIsSubmitting(true);
    try {
      const res = await loginWithPassphrase(username, passphrase);
      if (res.ok) {
        useWorldStreamStore.getState().restoreForAuthenticatedSession();
        setPassphrase("");
        props?.onClose?.();
      }
    } finally {
      setIsSubmitting(false);
    }
  };

  const handleDismiss = (): void => {
    setAdminAuthState({ loginPromptOpen: false, lastAuthError: null });
    props?.onClose?.();
  };

  return (
    <div role="dialog" aria-modal="true" aria-label="World Admin Passphrase Login" data-testid="admin-login-modal" style={MODAL_BACKDROP_STYLE}>
      {renderAdminLoginFormBody({
        username,
        passphrase,
        isSubmitting,
        authRequired: authSnap.authRequired,
        lastAuthError: authSnap.lastAuthError,
        onUsernameChange: setUsername,
        onPassphraseChange: setPassphrase,
        onDismiss: handleDismiss,
        onSubmit: (e) => { void handleSubmit(e); },
      })}
    </div>
  );
}

export default AdminLoginModal;
