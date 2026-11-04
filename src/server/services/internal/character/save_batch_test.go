// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"errors"
	"strings"
	"testing"
)

func buildMultiSubsystemDiffBatch(charID string) InventoryDiffSave {
	mutatedSlot := item("", 0, 7004, guid(14))
	mutatedSlot.Quantity = 12
	mutatedSlot.Durability = 85.5
	return InventoryDiffSave{
		CharacterID:  charID,
		FencingToken: 15,
		Upserts: []Item{
			item(Backpack, 1, 7001, guid(11)),
			item(Resource0, 5, 7002, guid(12)),
		},
		Deletes: []SlotKey{
			{Container: Equipment, Slot: 2},
		},
		Mutations: []SlotMutation{
			{Container: Backpack, Slot: 4, Deleted: false, Item: &mutatedSlot},
			{Container: Resource1, Slot: 7, Deleted: true},
		},
		Wallet: []WalletEntry{
			{CurrencyGuid: CopperCurrency, Amount: 50_000},
			{CurrencyGuid: 77, Amount: 250},
		},
		Jobs: []ProcessingJob{{
			JobGUID:       guid(30),
			StationKey:    "0123456789abcdef",
			RecipeGUID:    501,
			SubRecipeGUID: 0,
			Quantity:      5,
			QueuedAt:      created,
			StartedAt:     created,
			DurationMs:    60_000,
			ClientTxn:     guid(31),
			RatingXP:      120,
		}},
		KnownRecipes: []int64{1001, 1002},
		Warehouse: []WarehouseItem{{
			Area:          WarehouseItems,
			NodeGUID:      0,
			ContainerGUID: 10,
			Slot:          0,
			Item:          item(Backpack, 0, 7009, guid(40)),
		}},
	}
}

func TestSaveBatchCoalescedDirtySlotsAndSubsystems(t *testing.T) {
	ctx := context.Background()
	charID := strings.Repeat("d", 32)
	tx := &fakeDiffTx{tokens: map[string]int64{charID: 14}}
	diff := buildMultiSubsystemDiffBatch(charID)

	upserts, deletes, err := validateInventoryDiff(diff)
	if err != nil {
		t.Fatalf("validateInventoryDiff: %v", err)
	}
	if len(upserts) != 3 || len(deletes) != 2 {
		t.Fatalf("coalesced slots = (%d upserts, %d deletes), want (3, 2)", len(upserts), len(deletes))
	}
	if upserts[2].Container != Backpack || upserts[2].Slot != 4 || upserts[2].Quantity != 12 {
		t.Fatalf("coalesced mutation item = %+v, want Backpack/4 qty=12", upserts[2])
	}
	if err := applyInventoryDiffTx(ctx, tx, diff, upserts, deletes); err != nil {
		t.Fatalf("applyInventoryDiffTx: %v", err)
	}
	if tx.tokens[charID] != 15 {
		t.Fatalf("fencing token = %d, want 15", tx.tokens[charID])
	}
	wantPrefixes := []string{
		"WITH locked AS",
		"DELETE FROM character_items",
		"DELETE FROM character_resource_items",
		"INSERT INTO character_items",
		"INSERT INTO character_resource_items",
		"INSERT INTO character_wallet",
		"DELETE FROM character_warehouse_items",
		"INSERT INTO character_warehouse_items",
	}
	if len(tx.queries) != len(wantPrefixes) {
		t.Fatalf("executed %d SQL statements, want %d", len(tx.queries), len(wantPrefixes))
	}
	for i, prefix := range wantPrefixes {
		if !strings.HasPrefix(tx.queries[i], prefix) {
			t.Errorf("query[%d] = %q, want prefix %q", i, tx.queries[i], prefix)
		}
	}
}

func TestSaveBatchFencingTokenMonotonicityAndStaleReject(t *testing.T) {
	ctx := context.Background()
	charID := strings.Repeat("e", 32)
	unknownID := strings.Repeat("f", 32)
	slots := []Item{item(Backpack, 0, 7001, guid(1))}

	if _, _, err := validateInventoryDiff(InventoryDiffSave{
		CharacterID:  charID,
		FencingToken: -1,
		Upserts:      slots,
	}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("negative fencing token: got %v, want ErrInvalid", err)
	}

	tx := &fakeDiffTx{tokens: map[string]int64{charID: 10}}
	cases := []struct {
		name      string
		id        string
		token     int64
		wantErr   error
		wantCount int
		wantToken int64
	}{
		{"unknown character", unknownID, 10, ErrNotFound, 1, 10},
		{"equal token idempotent", charID, 10, nil, 2, 10},
		{"monotonic advance", charID, 12, nil, 2, 12},
		{"stale token rejected before writes", charID, 11, ErrStaleFencingToken, 1, 12},
	}
	for _, tc := range cases {
		tx.queries = nil
		diff := InventoryDiffSave{
			CharacterID:  tc.id,
			FencingToken: tc.token,
			Upserts:      slots,
			Wallet:       []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 100}},
		}
		upserts, deletes, err := validateInventoryDiff(diff)
		if err != nil {
			t.Fatalf("%s validateInventoryDiff: %v", tc.name, err)
		}
		if tc.wantErr == nil {
			diff.Wallet = nil
		}
		err = applyInventoryDiffTx(ctx, tx, diff, upserts, deletes)
		if !errors.Is(err, tc.wantErr) {
			t.Fatalf("%s: got err %v, want %v", tc.name, err, tc.wantErr)
		}
		if len(tx.queries) != tc.wantCount {
			t.Fatalf("%s: query count = %d, want %d", tc.name, len(tx.queries), tc.wantCount)
		}
		if tx.tokens[charID] != tc.wantToken {
			t.Fatalf("%s: persisted token = %d, want %d", tc.name, tx.tokens[charID], tc.wantToken)
		}
	}
}

func TestSaveBatchValidationRejectsDuplicateAndInvalidDeletes(t *testing.T) {
	ctx := context.Background()
	charID := strings.Repeat("a", 32)
	good := item(Backpack, 3, 7001, guid(1))
	tooManyDeletes := make([]SlotKey, MaxItems+1)
	for i := range tooManyDeletes {
		tooManyDeletes[i] = SlotKey{Container: Backpack, Slot: int32(i % 70)}
	}
	cases := []struct {
		name string
		diff InventoryDiffSave
		want error
	}{
		{"negative delete slot", InventoryDiffSave{CharacterID: charID, Deletes: []SlotKey{{Container: Backpack, Slot: -1}}}, ErrInvalid},
		{"delete slot past backpack limit", InventoryDiffSave{CharacterID: charID, Deletes: []SlotKey{{Container: Backpack, Slot: 70}}}, ErrInvalid},
		{"delete slot past resource limit", InventoryDiffSave{CharacterID: charID, Deletes: []SlotKey{{Container: Resource0, Slot: MaxResourceSlots}}}, ErrInvalid},
		{"delete unknown container", InventoryDiffSave{CharacterID: charID, Deletes: []SlotKey{{Container: "Stash", Slot: 0}}}, ErrInvalid},
		{"duplicate delete key", InventoryDiffSave{CharacterID: charID, Deletes: []SlotKey{{Container: Quest, Slot: 5}, {Container: Quest, Slot: 5}}}, ErrInvalid},
		{"upsert and delete conflict", InventoryDiffSave{CharacterID: charID, Upserts: []Item{good}, Deletes: []SlotKey{{Container: Backpack, Slot: 3}}}, ErrInvalid},
		{"mutation delete conflict with upsert", InventoryDiffSave{CharacterID: charID, Upserts: []Item{good}, Mutations: []SlotMutation{{Container: Backpack, Slot: 3, Deleted: true}}}, ErrInvalid},
		{"mutation upsert nil item", InventoryDiffSave{CharacterID: charID, Mutations: []SlotMutation{{Container: Backpack, Slot: 1, Deleted: false}}}, ErrInvalid},
		{"deletes exceed MaxItems", InventoryDiffSave{CharacterID: charID, Deletes: tooManyDeletes}, ErrInvalid},
	}
	for _, tc := range cases {
		if _, _, err := validateInventoryDiff(tc.diff); !errors.Is(err, tc.want) {
			t.Errorf("%s: got %v, want %v", tc.name, err, tc.want)
		}
	}

	empty := InventoryDiffSave{CharacterID: charID, FencingToken: 1}
	upserts, deletes, err := validateInventoryDiff(empty)
	if err != nil || len(upserts) != 0 || len(deletes) != 0 {
		t.Fatalf("empty diff validate = (%d, %d, %v), want (0, 0, nil)", len(upserts), len(deletes), err)
	}
	tx := &fakeDiffTx{tokens: map[string]int64{charID: 1}}
	if err := applyInventoryDiffTx(ctx, tx, empty, upserts, deletes); err != nil || len(tx.queries) != 1 {
		t.Fatalf("empty diff apply = (%v, %d queries), want (nil, 1)", err, len(tx.queries))
	}
}
