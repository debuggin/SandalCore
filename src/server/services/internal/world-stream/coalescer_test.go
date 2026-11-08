// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package worldstream

import (
	"testing"
	"time"
)

func TestCoalescerServiceTickHzAndNewCoalescer(t *testing.T) {
	t.Setenv("AOC_SERVICE_TICK_HZ", "")
	if got := ServiceTickHz(); got != 30.0 {
		t.Fatalf("expected default ServiceTickHz() == 30.0, got %v", got)
	}
	cDefault := NewCoalescer(0)
	if cDefault.tickInterval != DefaultTickInterval {
		t.Fatalf("expected default tickInterval %v, got %v", DefaultTickInterval, cDefault.tickInterval)
	}

	for _, tc := range []struct {
		envVal string
		wantHz float64
	}{
		{"5", 5.0},
		{"10", 10.0},
		{"20", 20.0},
		{"30", 30.0},
		{"1", 1.0},
		{"240", 240.0},
		{"0", 30.0},
		{"-5", 30.0},
		{"241", 30.0},
		{"not-a-number", 30.0},
	} {
		t.Setenv("AOC_SERVICE_TICK_HZ", tc.envVal)
		if got := ServiceTickHz(); got != tc.wantHz {
			t.Fatalf("ServiceTickHz(%q) = %v, want %v", tc.envVal, got, tc.wantHz)
		}
		c := NewCoalescer(0)
		wantInterval := time.Duration(float64(time.Second) / tc.wantHz)
		if tc.wantHz == 30.0 {
			wantInterval = DefaultTickInterval
		}
		if c.tickInterval != wantInterval {
			t.Fatalf("NewCoalescer(0) with Hz=%v tickInterval=%v, want %v", tc.wantHz, c.tickInterval, wantInterval)
		}
	}

	cExplicit := NewCoalescer(50 * time.Millisecond)
	if cExplicit.tickInterval != 50*time.Millisecond {
		t.Fatalf("expected explicit 50ms tickInterval, got %v", cExplicit.tickInterval)
	}
}

func TestCoalescerViewportAABBFilteringAndSnapshot(t *testing.T) {
	c := NewCoalescer(DefaultTickInterval)
	fixedNow := time.Date(2026, 11, 7, 12, 0, 0, 0, time.UTC)
	c.now = func() time.Time { return fixedNow }

	c.UpsertEntity(EntitySnapshot{ID: ""})
	if c.EntityCount() != 0 {
		t.Fatalf("expected empty ID upsert to be ignored, got count=%d", c.EntityCount())
	}

	entities := []EntitySnapshot{
		{ID: "riverlands:npc:10", Kind: "npc", Name: "Winstead Guard", Zone: "riverlands", X: -600000, Y: 300000, Level: 25},
		{ID: "riverlands:npc:20", Kind: "npc", Name: "Border Scout", Zone: "riverlands", X: -500000, Y: 400000, Level: 25},
		{ID: "riverlands:npc:30", Kind: "npc", Name: "Outside West", Zone: "riverlands", X: -600001, Y: 350000, Level: 25},
		{ID: "riverlands:npc:40", Kind: "npc", Name: "Outside North", Zone: "riverlands", X: -550000, Y: 400001, Level: 25},
		{ID: "sandsquall:monster:50", Kind: "monster", Name: "Desert Scorpion", Zone: "sandsquall", X: -550000, Y: 350000, Level: 30},
	}
	for _, e := range entities {
		c.UpsertEntity(e)
	}
	if c.EntityCount() != 5 {
		t.Fatalf("expected 5 entities, got %d", c.EntityCount())
	}

	boxFilter := ViewportFilter{
		Zone: "riverlands", HasBounds: true,
		MinX: -600000, MinY: 300000, MaxX: -500000, MaxY: 400000,
	}
	snap := c.Snapshot(boxFilter)
	if len(snap) != 2 || snap[0].ID != "riverlands:npc:10" || snap[1].ID != "riverlands:npc:20" {
		t.Fatalf("unexpected AABB filtered snapshot: %#v", snap)
	}
	if snap[0].UpdatedAt != fixedNow.UnixMilli() {
		t.Fatalf("expected injected clock UpdatedAt=%d, got %d", fixedNow.UnixMilli(), snap[0].UpdatedAt)
	}
}

func TestCoalescerUpsertRemoveSubscriptionAndDeltaMerging(t *testing.T) {
	c := NewCoalescer(DefaultTickInterval)
	nowMs := int64(1762500000000)
	c.now = func() time.Time { return time.UnixMilli(nowMs) }

	c.UpsertEntity(EntitySnapshot{ID: "player:1", Kind: "player", Name: "TST CoalescerProbe", Zone: "riverlands", X: -644519, Y: 376773, HealthPct: 100, Level: 50})
	filter := ViewportFilter{Zone: "riverlands", HasBounds: true, MinX: -700000, MinY: 300000, MaxX: -600000, MaxY: 400000}
	sub := c.Subscribe("sub-1", filter)
	if c.SubscriberCount() != 1 {
		t.Fatalf("expected 1 subscriber, got %d", c.SubscriberCount())
	}

	if flushed := c.Flush(); flushed != 1 {
		t.Fatalf("expected 1 flushed subscription on initial state, got %d", flushed)
	}
	d1 := <-sub.Updates()
	if d1.Sequence != 1 || len(d1.Added) != 1 || d1.Added[0].ID != "player:1" {
		t.Fatalf("unexpected first delta: %#v", d1)
	}

	c.UpsertEntity(EntitySnapshot{ID: "player:1", Kind: "player", Name: "TST CoalescerProbe", Zone: "riverlands", X: -640000, Y: 375000, HealthPct: 90, Level: 50})
	c.UpsertEntity(EntitySnapshot{ID: "npc:2", Kind: "npc", Name: "Quartermaster", Zone: "riverlands", X: -650000, Y: 350000, HealthPct: 100, Level: 30})
	c.Flush()

	c.RemoveEntity("npc:2")
	c.UpsertEntity(EntitySnapshot{ID: "player:1", Kind: "player", Name: "TST CoalescerProbe", Zone: "riverlands", X: -100000, Y: 100000, HealthPct: 90, Level: 50})
	c.Flush()

	if sub.DroppedCount() != 1 {
		t.Fatalf("expected 1 dropped/merged tick for unread channel, got %d", sub.DroppedCount())
	}
	d2 := <-sub.Updates()
	if d2.Sequence != 3 || d2.DroppedTicks != 1 || len(d2.Added) != 0 || len(d2.Removed) != 1 || d2.Removed[0] != "player:1" {
		t.Fatalf("unexpected merged delta after viewport exit & remove: %#v", d2)
	}

	c.RemoveEntity("player:1")
	c.Unsubscribe("sub-1")
	if c.SubscriberCount() != 0 || c.EntityCount() != 0 {
		t.Fatalf("expected 0 subscribers and 0 entities after cleanup, got subs=%d entities=%d", c.SubscriberCount(), c.EntityCount())
	}
}
