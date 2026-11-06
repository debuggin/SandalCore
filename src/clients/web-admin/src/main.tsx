// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";
import { createRoot } from "react-dom/client";
import { LogsAndContainersTab } from "./components/LogsAndContainersTab";
import { PlayersAndSpawnerTab } from "./components/PlayersAndSpawnerTab";
import { VerraMapCanvas } from "./map/VerraMapCanvas";
import { type AdminTabId, useWorldStreamStore } from "./state/worldStreamStore";

export interface AdminTabDefinition {
  readonly id: AdminTabId;
  readonly label: string;
}

export const ADMIN_TABS: readonly AdminTabDefinition[] = Object.freeze([
  { id: "map", label: "World Map" },
  { id: "players-spawner", label: "Players & Spawner" },
  { id: "logs-containers", label: "Logs & Container Analytics" },
]);

export function App(): React.ReactElement {
  const storeSnap = useWorldStreamStore();
  return (
    <div
      data-testid="sandalcore-web-admin-app"
      style={{
        display: "flex",
        flexDirection: "column",
        minHeight: "100vh",
        backgroundColor: "#090D16",
        color: "#F0F6FC",
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
          padding: "8px 16px",
          borderBottom: "1px solid #30363D",
          backgroundColor: "#0D1117",
        }}
      >
        <div style={{ fontWeight: 700, fontSize: "14px", color: "#F0F6FC" }}>
          SandalCore GM Studio
        </div>
        <div role="tablist" aria-label="Admin Workspace Tabs" style={{ display: "flex", gap: "8px" }}>
          {ADMIN_TABS.map((tab) => {
            const isActive = storeSnap.activeTab === tab.id;
            return (
              <button
                key={tab.id}
                type="button"
                role="tab"
                aria-selected={isActive}
                data-testid={`admin-tab-${tab.id}`}
                onClick={() => storeSnap.setActiveTab(tab.id)}
                style={{
                  padding: "6px 12px",
                  borderRadius: "6px",
                  border: "1px solid #30363D",
                  backgroundColor: isActive ? "#1F6FEB" : "#21262D",
                  color: "#F0F6FC",
                  fontSize: "12px",
                  fontWeight: 600,
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
        {storeSnap.activeTab === "map" ? (
          <VerraMapCanvas />
        ) : storeSnap.activeTab === "players-spawner" ? (
          <PlayersAndSpawnerTab />
        ) : (
          <LogsAndContainersTab />
        )}
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
