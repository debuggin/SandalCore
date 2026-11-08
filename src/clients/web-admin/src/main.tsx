// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import { createRoot } from "react-dom/client";
import {
  AdminAuthSessionBadge,
  AdminLoginModal,
  END_SESSION_BUTTON_LABEL,
  END_SESSION_BUTTON_STYLE,
} from "./components/AdminLoginModal";
import { LogsAndContainersTab } from "./components/LogsAndContainersTab";
import {
  PlayersAndSpawnerTab,
  SANDAL_LAUNCHER_TOKENS,
} from "./components/PlayersAndSpawnerTab";
import { VerraMapCanvas } from "./map/VerraMapCanvas";
import {
  DEFAULT_ADMIN_USERNAME,
  installGlobalAdminAuthFetchInterceptor,
  normalizeAdminUsername,
} from "./state/adminAuthClient";
import {
  type AdminTabId,
  parseModalFromUrlHash,
  parseTabFromUrlHash,
  useWorldStreamStore,
} from "./state/worldStreamStore";

installGlobalAdminAuthFetchInterceptor();

export interface AdminTabDefinition {
  readonly id: AdminTabId;
  readonly label: string;
}

export const ADMIN_TABS: readonly AdminTabDefinition[] = Object.freeze([
  { id: "map", label: "Map" },
  { id: "analytics", label: "Observability" },
]);

interface ErrorBoundaryProps {
  readonly children?: React.ReactNode;
}

interface ErrorBoundaryState {
  readonly hasError: boolean;
  readonly message: string;
}

export class WebAdminErrorBoundary extends React.Component<
  ErrorBoundaryProps,
  ErrorBoundaryState
> {
  constructor(props: ErrorBoundaryProps) {
    super(props);
    this.state = { hasError: false, message: "" };
  }

  static getDerivedStateFromError(err: unknown): ErrorBoundaryState {
    return {
      hasError: true,
      message: err instanceof Error ? err.message : String(err),
    };
  }

  render(): React.ReactNode {
    if (this.state.hasError) {
      return (
        <div
          role="alert"
          data-testid="web-admin-error-boundary"
          style={{
            padding: "24px",
            margin: "16px",
            borderRadius: "10px",
            border: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
            background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush,
            color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100,
          }}
        >
          <strong style={{ color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500 }}>
            Workspace Recovered from Render Error
          </strong>
          <div style={{ marginTop: "8px", fontSize: "12px" }}>
            {this.state.message}
          </div>
          <button
            type="button"
            onClick={() => this.setState({ hasError: false, message: "" })}
            style={{
              marginTop: "12px",
              minHeight: "32px",
              padding: "6px 14px",
              borderRadius: "6px",
              border: `1px solid ${SANDAL_LAUNCHER_TOKENS.VerraRelicGold500}`,
              backgroundColor: SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush,
              color: SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400,
              cursor: "pointer",
            }}
          >
            Resume Workspace
          </button>
        </div>
      );
    }
    return this.props.children ?? null;
  }
}

function renderAdminTabButtons(
  activeTab: AdminTabId,
  isAnalyticsUnified: boolean,
  onSelectTab: (tabId: AdminTabId) => void,
): React.ReactElement {
  return (
    <div role="tablist" aria-label="Admin Workspace Tabs" style={{ display: "flex", gap: "8px" }}>
      {ADMIN_TABS.map((tab) => {
        const isActive = tab.id === "analytics" ? isAnalyticsUnified : activeTab === tab.id;
        return (
          <button
            key={tab.id}
            type="button"
            role="tab"
            aria-selected={isActive}
            data-testid={`admin-tab-${tab.id}`}
            onClick={() => onSelectTab(tab.id)}
            style={{
              minHeight: "34px",
              padding: "6px 14px",
              borderRadius: "8px",
              border: isActive
                ? `1px solid ${SANDAL_LAUNCHER_TOKENS.VerraRelicGold500}`
                : `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
              backgroundColor: isActive
                ? "rgba(232, 178, 90, 0.2)"
                : SANDAL_LAUNCHER_TOKENS.AoCSecondaryBtnBrush,
              color: isActive
                ? SANDAL_LAUNCHER_TOKENS.VerraChampagneGold400
                : SANDAL_LAUNCHER_TOKENS.VerraAlabaster100,
              fontSize: "12px",
              fontWeight: 700,
              letterSpacing: "0.06em",
              textTransform: "uppercase",
              cursor: "pointer",
            }}
          >
            {tab.label}
          </button>
        );
      })}
    </div>
  );
}

function renderAdminTopBar(
  activeTab: AdminTabId,
  isAnalyticsUnified: boolean,
  onSelectTab: (tabId: AdminTabId) => void,
): React.ReactElement {
  return (
    <nav
      aria-label="SandalCore Web Admin Navigation"
      data-testid="web-admin-tab-bar"
      style={{
        display: "flex",
        alignItems: "center",
        justifyContent: "space-between",
        flexWrap: "wrap",
        gap: "10px",
        padding: "8px 18px",
        borderBottom: `1px solid ${SANDAL_LAUNCHER_TOKENS.AoCCardBorderBrush}`,
        background: SANDAL_LAUNCHER_TOKENS.AoCFiligreeDeckBrush,
        backdropFilter: "blur(16px)",
      }}
    >
      <div
        data-testid="web-admin-brand-title"
        style={{
          fontWeight: 700,
          fontSize: "13px",
          letterSpacing: "0.14em",
          textTransform: "uppercase",
          color: SANDAL_LAUNCHER_TOKENS.VerraRelicGold500,
        }}
      >
        SANDALCORE
      </div>
      <div style={{ display: "flex", alignItems: "center", gap: "12px", flexWrap: "wrap" }}>
        {renderAdminTabButtons(activeTab, isAnalyticsUnified, onSelectTab)}
        <AdminAuthSessionBadge />
      </div>
    </nav>
  );
}

export function App(): React.ReactElement {
  const storeSnap = useWorldStreamStore();
  React.useEffect(() => {
    if (typeof window === "undefined") return;
    const onHashChange = (): void => {
      const nextTab = parseTabFromUrlHash(window.location.hash);
      if (nextTab && nextTab !== storeSnap.activeTab) {
        storeSnap.setActiveTab(nextTab);
      }
      const nextModal = parseModalFromUrlHash(window.location.hash);
      if (nextModal !== undefined && nextModal !== storeSnap.activeModal) {
        storeSnap.setActiveModal(nextModal);
      }
    };
    onHashChange();
    window.addEventListener("hashchange", onHashChange);
    return () => {
      window.removeEventListener("hashchange", onHashChange);
    };
  }, [storeSnap.activeTab, storeSnap.activeModal]);

  const isAnalyticsUnified =
    storeSnap.activeTab === "analytics" ||
    storeSnap.activeTab === "logs" ||
    storeSnap.activeTab === "logs-containers";

  return (
    <div
      data-testid="sandalcore-web-admin-app"
      style={{
        display: "flex",
        flexDirection: "column",
        width: "100%",
        height: "100vh",
        maxHeight: "100dvh",
        boxSizing: "border-box",
        overflow: "hidden",
        scrollbarGutter: "stable",
        backgroundColor: SANDAL_LAUNCHER_TOKENS.VerraObsidian100,
        color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100,
        fontFamily: '"Segoe UI", system-ui, -apple-system, sans-serif',
      }}
    >
      {renderAdminTopBar(storeSnap.activeTab, isAnalyticsUnified, storeSnap.setActiveTab)}
      <main
        data-testid="web-admin-main-view"
        style={{
          flex: 1,
          minHeight: 0,
          overflow: storeSnap.activeTab === "map" ? "hidden" : "auto",
          display: "flex",
          flexDirection: "column",
          boxSizing: "border-box",
          scrollbarGutter: "stable",
        }}
      >
        <WebAdminErrorBoundary>
          {storeSnap.activeTab === "map" ? (
            <VerraMapCanvas />
          ) : storeSnap.activeTab === "players-spawner" ? (
            <PlayersAndSpawnerTab />
          ) : (
            <LogsAndContainersTab mode="both" />
          )}
        </WebAdminErrorBoundary>
      </main>
      <AdminLoginModal />
    </div>
  );
}

if (typeof document !== "undefined") {
  if (document.documentElement?.style) {
    document.documentElement.style.scrollbarGutter = "stable";
    document.documentElement.style.overflow = "hidden";
    document.documentElement.style.height = "100%";
    document.documentElement.style.margin = "0";
  }
  if (document.body?.style) {
    document.body.style.overflow = "hidden";
    document.body.style.height = "100%";
    document.body.style.margin = "0";
  }
  const rootElement = document.getElementById("root");
  if (rootElement) {
    createRoot(rootElement).render(
      React.createElement(React.StrictMode, null, React.createElement(App)),
    );
  }
}

export { DEFAULT_ADMIN_USERNAME, END_SESSION_BUTTON_LABEL, END_SESSION_BUTTON_STYLE, normalizeAdminUsername };
export default App;
