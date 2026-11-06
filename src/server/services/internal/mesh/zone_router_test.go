// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package mesh

import (
	"testing"
	"time"
)

func newTwoZoneTestRouter(t *testing.T, hysteresisCm float64) *ZoneRouter {
	t.Helper()
	r := NewZoneRouter(hysteresisCm)
	if err := r.UpsertZone(ZoneBounds{
		ZoneID:         1,
		MinX:           -1000000.0,
		MaxX:           0.0,
		MinY:           -1000000.0,
		MaxY:           0.0,
		ServerID:       101,
		Address:        "127.0.0.1:7777",
		AuthorityEpoch: 1,
	}); err != nil {
		t.Fatalf("UpsertZone(1) failed: %v", err)
	}
	if err := r.UpsertZone(ZoneBounds{
		ZoneID:         2,
		MinX:           -1000000.0,
		MaxX:           0.0,
		MinY:           0.0,
		MaxY:           1000000.0,
		ServerID:       102,
		Address:        "127.0.0.1:7778",
		AuthorityEpoch: 1,
	}); err != nil {
		t.Fatalf("UpsertZone(2) failed: %v", err)
	}
	return r
}

func TestResolveAuthorityZoneWithCooldownSuppressesBoundaryJitter(t *testing.T) {
	r := newTwoZoneTestRouter(t, 5000.0)
	baseTime := time.Date(2026, 11, 6, 12, 0, 0, 0, time.UTC)

	jitterY := []float64{
		-1500.0, 2200.0, -800.0, 4900.0, -300.0,
		3500.0, -1200.0, 4100.0, -50.0, 2800.0,
	}
	currZone := "riverlands"
	for i, y := range jitterY {
		now := baseTime.Add(time.Duration(i*400) * time.Millisecond)
		zoneID, handedOver, suppressed := r.ResolveAuthorityZoneWithCooldown("player:jitter", currZone, -450000.0, y, now)
		if handedOver {
			t.Fatalf("sample %d (y=%f): unexpected handover to %q during boundary jitter", i, y, zoneID)
		}
		if zoneID != "riverlands" {
			t.Fatalf("sample %d (y=%f): expected zoneID=riverlands, got %q", i, y, zoneID)
		}
		if y > 0 && !suppressed {
			t.Fatalf("sample %d (y=%f): expected suppressedByHysteresis=true inside +5000cm hysteresis band", i, y)
		}
	}
}

func TestResolveAuthorityZoneWithCooldownDwellAndCooldownWindow(t *testing.T) {
	r := newTwoZoneTestRouter(t, 5000.0)
	t0 := time.Date(2026, 11, 6, 12, 5, 0, 0, time.UTC)

	// Fewer than 3 samples deep in sandsquall followed by stepping back inside riverlands must not hand over.
	for i := 0; i < 2; i++ {
		z, handed, suppressed := r.ResolveAuthorityZoneWithCooldown(
			"player:dwell", "riverlands", -450000.0, 25000.0, t0.Add(time.Duration(i*300)*time.Millisecond),
		)
		if handed || !suppressed || z != "riverlands" {
			t.Fatalf("pre-dwell step %d: got zone=%q handed=%v suppressed=%v", i, z, handed, suppressed)
		}
	}
	zBack, handedBack, suppressedBack := r.ResolveAuthorityZoneWithCooldown(
		"player:dwell", "riverlands", -450000.0, -10000.0, t0.Add(700*time.Millisecond),
	)
	if handedBack || suppressedBack || zBack != "riverlands" {
		t.Fatalf("return step: got zone=%q handed=%v suppressed=%v", zBack, handedBack, suppressedBack)
	}

	// 3 consecutive samples beyond hysteresis band commit handover on sample 3.
	tCommitBase := t0.Add(1 * time.Second)
	for i := 1; i <= 3; i++ {
		now := tCommitBase.Add(time.Duration(i*300) * time.Millisecond)
		z, handed, suppressed := r.ResolveAuthorityZoneWithCooldown("player:dwell", "riverlands", -450000.0, 35000.0, now)
		if i < 3 {
			if handed || !suppressed || z != "riverlands" {
				t.Fatalf("dwell sample %d: expected riverlands suppressed, got zone=%q handed=%v suppressed=%v", i, z, handed, suppressed)
			}
			continue
		}
		if !handed || suppressed || z != "sandsquall" {
			t.Fatalf("dwell sample 3: expected committed handover to sandsquall, got zone=%q handed=%v suppressed=%v", z, handed, suppressed)
		}
	}

	// Immediate (<2s) bounce back deep into riverlands must be suppressed by the 2s temporal cooldown.
	lastCommitAt := tCommitBase.Add(900 * time.Millisecond)
	for i := 1; i <= 4; i++ {
		now := lastCommitAt.Add(time.Duration(i*300) * time.Millisecond)
		z, handed, suppressed := r.ResolveAuthorityZoneWithCooldown("player:dwell", "sandsquall", -450000.0, -40000.0, now)
		if handed || !suppressed || z != "sandsquall" {
			t.Fatalf("cooldown sample %d (+%v): expected cooldown suppression on sandsquall, got zone=%q handed=%v suppressed=%v",
				i, now.Sub(lastCommitAt), z, handed, suppressed)
		}
	}

	// After >=2s cooldown has elapsed, 3 consecutive samples commit the return handover cleanly.
	afterCooldown := lastCommitAt.Add(2100 * time.Millisecond)
	for i := 1; i <= 3; i++ {
		now := afterCooldown.Add(time.Duration(i*200) * time.Millisecond)
		z, handed, suppressed := r.ResolveAuthorityZoneWithCooldown("player:dwell", "sandsquall", -450000.0, -40000.0, now)
		if i < 3 && (handed || !suppressed || z != "sandsquall") {
			t.Fatalf("post-cooldown dwell %d: got zone=%q handed=%v suppressed=%v", i, z, handed, suppressed)
		}
		if i == 3 && (!handed || suppressed || z != "riverlands") {
			t.Fatalf("post-cooldown dwell 3: expected handover back to riverlands, got zone=%q handed=%v suppressed=%v", z, handed, suppressed)
		}
	}
}
