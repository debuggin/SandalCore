// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admingateway

import (
	"encoding/json"
	"net/http"
	"strings"
	"testing"
	"time"
)

func TestFunctionalApiCoreCatalogAndHealthContracts(t *testing.T) {
	fixedNow := time.Date(2026, 11, 7, 15, 30, 0, 0, time.UTC)
	srv := NewRouter(Config{
		Now:               func() time.Time { return fixedNow },
		WorldAdminClient:  &fakeWorldAdminClient{},
		WorldStreamClient: &fakeWorldStreamClient{},
		CityNodeClient:    &fakeCityNodeClient{},
	})

	for _, ep := range []string{
		"/healthz",
		"/api/v1/catalog/places?page=1&page_size=10",
		"/api/v1/catalog/npcs?page=1&page_size=10",
		"/api/v1/catalog/items?page=1&page_size=10",
		"/api/v1/gm/commands",
		"/api/v1/gm/audit",
	} {
		rec := serveAuthed(srv, http.MethodGet, ep, "gm-observer-dev-token", map[string]string{"Accept": "application/json"}, nil)
		if rec.Code != http.StatusOK {
			t.Fatalf("GET %s status = %d, want 200; body = %s", ep, rec.Code, rec.Body.String())
		}
		if !strings.HasPrefix(rec.Header().Get("ETag"), `"sha256-`) {
			t.Fatalf("GET %s missing strong SHA-256 ETag, got %q", ep, rec.Header().Get("ETag"))
		}
	}

	playersRec := serveAuthed(srv, http.MethodGet, "/api/v1/world/state?layers=players", "gm-observer-dev-token", map[string]string{"Accept": "application/json"}, nil)
	if playersRec.Code != http.StatusOK {
		t.Fatalf("GET /api/v1/world/state?layers=players status = %d", playersRec.Code)
	}
	var payload map[string]any
	if err := json.Unmarshal(playersRec.Body.Bytes(), &payload); err != nil {
		t.Fatalf("decode /api/v1/world/state: %v", err)
	}
	players, _ := payload["players"].([]any)
	if len(players) != 1 {
		t.Fatalf("expected 1 player from fakeWorldAdminClient, got %d", len(players))
	}
}
