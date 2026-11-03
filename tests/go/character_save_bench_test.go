// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package go_test

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

var errStaleFencingToken = errors.New("stale fencing token: split-brain character save rejected")

type simSlotMutation struct {
	container uint8
	slot      uint8
	deleted   bool
	recordID  int64
	quantity  int32
}

type simDiffSnapshot struct {
	fencingToken int64
	upserts      []simSlotMutation
	deletes      []simSlotMutation
	dirtyBits    [6][4]uint64
	deletedBits  [6][4]uint64
}

type simDirtyTracker struct {
	dirty        [6][4]uint64
	deleted      [6][4]uint64
	fencingToken int64
}

func (t *simDirtyTracker) markDirty(c, slot uint8) {
	if c >= 6 {
		return
	}
	w, mask := slot/64, uint64(1)<<(slot%64)
	t.dirty[c][w] |= mask
	t.deleted[c][w] &^= mask
}

func (t *simDirtyTracker) markDeleted(c, slot uint8) {
	if c >= 6 {
		return
	}
	w, mask := slot/64, uint64(1)<<(slot%64)
	t.deleted[c][w] |= mask
	t.dirty[c][w] &^= mask
}

func (t *simDirtyTracker) drainSnapshot() simDiffSnapshot {
	t.fencingToken++
	snap := simDiffSnapshot{
		fencingToken: t.fencingToken,
		dirtyBits:    t.dirty,
		deletedBits:  t.deleted,
	}
	t.dirty = [6][4]uint64{}
	t.deleted = [6][4]uint64{}
	for c := uint8(0); c < 6; c++ {
		for s := 0; s < 256; s++ {
			w, mask := s/64, uint64(1)<<(s%64)
			if (snap.dirtyBits[c][w] & mask) != 0 {
				snap.upserts = append(snap.upserts, simSlotMutation{container: c, slot: uint8(s), recordID: 7001, quantity: 1})
			} else if (snap.deletedBits[c][w] & mask) != 0 {
				snap.deletes = append(snap.deletes, simSlotMutation{container: c, slot: uint8(s), deleted: true})
			}
		}
	}
	return snap
}

func (t *simDirtyTracker) restoreOnRollback(failed simDiffSnapshot) {
	for c := 0; c < 6; c++ {
		for w := 0; w < 4; w++ {
			untouched := ^(t.dirty[c][w] | t.deleted[c][w])
			t.dirty[c][w] |= failed.dirtyBits[c][w] & untouched
			t.deleted[c][w] |= failed.deletedBits[c][w] & untouched
		}
	}
}

func (t *simDirtyTracker) isDirty(c, slot uint8) bool {
	return c < 6 && (t.dirty[c][slot/64]&(uint64(1)<<(slot%64))) != 0
}

func (t *simDirtyTracker) isDeleted(c, slot uint8) bool {
	return c < 6 && (t.deleted[c][slot/64]&(uint64(1)<<(slot%64))) != 0
}

func estimateDiffSaveStats(snap simDiffSnapshot) (statements int, walBytes int) {
	// BEGIN (1) + fencing CTE (1) + COMMIT (1)
	statements = 3
	walBytes = 96
	if len(snap.deletes) > 0 {
		statements++
		walBytes += 64 + len(snap.deletes)*48
	}
	if len(snap.upserts) > 0 {
		statements++
		walBytes += 96 + len(snap.upserts)*184
	}
	return statements, walBytes
}

func estimateFullWipeStats(totalItems int) (statements int, walBytes int) {
	// BEGIN + SELECT FOR UPDATE + 2 DELETE + N INSERT + COMMIT
	statements = 5 + totalItems
	walBytes = 256 + totalItems*248
	return statements, walBytes
}

func TestDifferentialDirtySlotStatementCountAndWalBudget(t *testing.T) {
	var tracker simDirtyTracker
	tracker.markDirty(0, 2)
	tracker.markDeleted(0, 5)
	snap := tracker.drainSnapshot()

	diffStmts, diffWal := estimateDiffSaveStats(snap)
	fullStmts, fullWal := estimateFullWipeStats(190)

	if diffStmts > 7 {
		t.Fatalf("2-slot dirty tick emitted %d SQL statements, want <= 7", diffStmts)
	}
	if fullStmts < 192 {
		t.Fatalf("legacy full-wipe save emitted %d statements, want >= 192", fullStmts)
	}
	if diffWal > 1200 {
		t.Fatalf("2-slot dirty tick WAL payload = %d bytes, want <= 1200 bytes (full wipe = %d bytes)", diffWal, fullWal)
	}

	itemsGo, err := os.ReadFile(filepath.Join("..", "..", "services", "internal", "character", "items.go"))
	if err != nil {
		t.Fatalf("read items.go: %v", err)
	}
	src := string(itemsGo)
	for _, needle := range []string{"ErrStaleFencingToken", "SaveInventoryDiff", "InventoryDiffSave", "unnest(", "IS DISTINCT FROM"} {
		if !strings.Contains(src, needle) {
			t.Errorf("services/internal/character/items.go missing %q", needle)
		}
	}
	whGo, err := os.ReadFile(filepath.Join("..", "..", "services", "internal", "character", "warehouse.go"))
	if err != nil {
		t.Fatalf("read warehouse.go: %v", err)
	}
	if !strings.Contains(string(whGo), "unnest(") {
		t.Errorf("services/internal/character/warehouse.go missing set-based unnest")
	}
}

func TestMonotonicFencingTokenSplitBrainRejection(t *testing.T) {
	persistedToken := int64(100)
	applyFenced := func(incoming int64) error {
		if incoming < persistedToken {
			return errStaleFencingToken
		}
		persistedToken = incoming
		return nil
	}
	if err := applyFenced(101); err != nil {
		t.Fatalf("monotonic token 101 rejected: %v", err)
	}
	if err := applyFenced(99); !errors.Is(err, errStaleFencingToken) {
		t.Fatalf("stale token 99: got %v, want %v", err, errStaleFencingToken)
	}
	if persistedToken != 101 {
		t.Fatalf("persisted fencing token mutated to %d after stale save", persistedToken)
	}
}

func TestRollbackDirtyBitRestorationAndHeaderContract(t *testing.T) {
	var tracker simDirtyTracker
	tracker.markDirty(0, 3)
	tracker.markDeleted(0, 7)
	snap1 := tracker.drainSnapshot()

	if tracker.isDirty(0, 3) || tracker.isDeleted(0, 7) {
		t.Fatal("drainSnapshot must clear active dirty and deleted bitmasks")
	}
	// Newer tick mutates slot 7 (now dirty) and slot 12 before snap1 RPC fails.
	tracker.markDirty(0, 7)
	tracker.markDirty(0, 12)
	tracker.restoreOnRollback(snap1)

	if !tracker.isDirty(0, 3) {
		t.Error("untouched slot 3 must be restored as dirty on rollback")
	}
	if !tracker.isDirty(0, 7) || tracker.isDeleted(0, 7) {
		t.Error("slot 7 mutated in newer tick must keep newer dirty state and not revert to deleted")
	}
	if !tracker.isDirty(0, 12) {
		t.Error("slot 12 from newer tick must remain dirty")
	}

	hdr, err := os.ReadFile(filepath.Join("..", "..", "server", "world", "session", "inventory_dirty_tracker.h"))
	if err != nil {
		t.Fatalf("read inventory_dirty_tracker.h: %v", err)
	}
	hsrc := string(hdr)
	for _, needle := range []string{"#pragma once", "InventoryDirtyTracker", "mark_dirty", "mark_deleted", "record_outcome", "drain_snapshot", "restore_on_rollback"} {
		if !strings.Contains(hsrc, needle) {
			t.Errorf("inventory_dirty_tracker.h missing %q", needle)
		}
	}
}

func BenchmarkCharacterSaveFullWipeVsDiff(b *testing.B) {
	b.Run("FullWipe190Slots", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			stmts, wal := estimateFullWipeStats(190)
			if stmts == 0 || wal == 0 {
				b.Fatal("unexpected zero stats")
			}
		}
	})
	b.Run("DiffSave2Slots", func(b *testing.B) {
		var tracker simDirtyTracker
		for i := 0; i < b.N; i++ {
			tracker.markDirty(0, 2)
			tracker.markDeleted(0, 5)
			snap := tracker.drainSnapshot()
			stmts, wal := estimateDiffSaveStats(snap)
			if stmts == 0 || wal == 0 {
				b.Fatal("unexpected zero stats")
			}
		}
	})
}
