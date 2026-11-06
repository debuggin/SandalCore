// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import { createRoot } from "react-dom/client";
import { LogsAndContainersTab } from "./components/LogsAndContainersTab";
import {
  PlayersAndSpawnerTab,
  SANDAL_LAUNCHER_TOKENS,
} from "./components/PlayersAndSpawnerTab";
import { VerraMapCanvas } from "./map/VerraMapCanvas";
import {
  type AdminTabId,
  parseModalFromUrlHash,
  parseTabFromUrlHash,
  useWorldStreamStore,
} from "./state/worldStreamStore";

export interface AdminTabDefinition {
  readonly id: AdminTabId;
  readonly label: string;
}

export const ADMIN_TABS: readonly AdminTabDefinition[] = Object.freeze([
  { id: "map", label: "Map" },
  { id: "analytics", label: "Analytics & Logs" },
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
        minHeight: "100vh",
        backgroundColor: SANDAL_LAUNCHER_TOKENS.VerraObsidian100,
        color: SANDAL_LAUNCHER_TOKENS.VerraAlabaster100,
        fontFamily: '"Segoe UI", system-ui, -apple-system, sans-serif',
      }}
    >
      <nav
        aria-label="SandalCore Web Admin Navigation"
        data-testid="web-admin-tab-bar"
        style={{
          display: "flex",
          alignItems: "center",
          justifyContent: "space-between",
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
        <div
          role="tablist"
          aria-label="Admin Workspace Tabs"
          style={{ display: "flex", gap: "8px" }}
        >
          {ADMIN_TABS.map((tab) => {
            const isActive =
              tab.id === "analytics"
                ? isAnalyticsUnified
                : storeSnap.activeTab === tab.id;
            return (
              <button
                key={tab.id}
                type="button"
                role="tab"
                aria-selected={isActive}
                data-testid={`admin-tab-${tab.id}`}
                onClick={() => storeSnap.setActiveTab(tab.id)}
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
      </nav>
      <main style={{ flex: 1, display: "flex", flexDirection: "column" }}>
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
    </div>
  );
}

if (typeof document !== "undefined") {
  const rootElement = document.getElementById("root");
  if (rootElement) {
    createRoot(rootElement).render(
      React.createElement(React.StrictMode, null, React.createElement(App)),
    );
  }
}

export default App;
