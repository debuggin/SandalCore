import React from "react";
import { createRoot } from "react-dom/client";
import { VerraMapCanvas } from "./map/VerraMapCanvas";

export const App: React.FC = () => {
  return <VerraMapCanvas />;
};

const rootElement = document.getElementById("root");
if (rootElement) {
  createRoot(rootElement).render(
    <React.StrictMode>
      <App />
    </React.StrictMode>,
  );
}
