// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"log/slog"
	"math"
	"reflect"
	"strings"
	"testing"
	"time"
)

// -- C | Y | B . C.O.A --

func verifySubMillisecondDurabilitySave(t *testing.T, ctx context.Context, s *Store, charID string) {
	t.Helper()
	odd := item(Backpack, 1, 5, guid(9))
	odd.Durability = 0.1
	odd.CreatedAt = time.UnixMilli(1_790_000_000_123)
	if err := s.SaveInventory(ctx, charID, []Item{odd}); err != nil {
		t.Fatal(err)
	}
	if got, _ := s.Inventory(ctx, charID); len(got) != 1 || got[0].Durability != 0.1 || got[0].CreatedAt.UnixMilli() != 1_790_000_000_123 {
		t.Errorf("durability and time: %+v", got)
	}
}

func TestSaveInventoryReplacesAllItemsAndTheLatestWins(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	moved := []Item{item(Backpack, 0, 6064630214426886486, guid(2)), item(Equipment, 15, 6064631131688402948, guid(3))}
	moved[0].Durability = 37.5
	moved[1].Quantity = 3
	moved[1].Quality = 51
	if err := s.SaveInventory(ctx, c.ID, moved); err != nil {
		t.Fatalf("SaveInventory: %v", err)
	}
	got, err := s.Inventory(ctx, c.ID)
	if err != nil || !reflect.DeepEqual(got, moved) {
		t.Errorf("after the save: %+v, %v; want %+v", got, err, moved)
	}
	latest := []Item{item(Equipment, 2, 6064630214426886486, guid(2))}
	if err := s.SaveInventory(ctx, c.ID, latest); err != nil {
		t.Fatal(err)
	}
	if got, _ := s.Inventory(ctx, c.ID); !reflect.DeepEqual(got, latest) {
		t.Errorf("after the second save: %+v", got)
	}
	if after, err := s.Get(ctx, c.ID); err != nil || after.Name != "cyb" || after.ClassID != 2 {
		t.Errorf("the character after saving its items: %+v, %v", after, err)
	}
	verifySubMillisecondDurabilitySave(t, ctx, s, c.ID)
}

func TestSaveInventoryRefusalsChangeNothing(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	want := func() []Item { got, _ := s.Inventory(ctx, c.ID); return got }()

	dupSlot := []Item{item(Backpack, 1, 1, guid(1)), item(Backpack, 1, 1, guid(2))}
	dupGuid := []Item{item(Backpack, 1, 1, guid(1)), item(Backpack, 2, 1, guid(1))}
	badContainer := []Item{item("Bank", 1, 1, guid(1))}
	badSlot := []Item{item(Backpack, 70, 1, guid(1))}
	badQuantity := item(Backpack, 1, 1, guid(1))
	badQuantity.Quantity = 0
	nan := item(Backpack, 1, 1, guid(1))
	nan.Durability = float32(math.NaN())
	noTime := item(Backpack, 1, 1, guid(1))
	noTime.CreatedAt = time.Time{}
	tooMany := make([]Item, MaxItems+1)
	for i := range tooMany {
		tooMany[i] = item(Backpack, int32(i%70), 1, guid(i+1))
	}
	for name, items := range map[string][]Item{
		"no items at all": nil, "an empty list": {}, "two items in a slot": dupSlot, "two items with a guid": dupGuid,
		"a container that is none": badContainer, "a slot out of range": badSlot, "quantity 0": {badQuantity},
		"a NaN durability": {nan}, "no creation time": {noTime}, "more items than slots": tooMany,
	} {
		if err := s.SaveInventory(ctx, c.ID, items); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: %v, want ErrInvalid", name, err)
		}
		if got, _ := s.Inventory(ctx, c.ID); !reflect.DeepEqual(got, want) {
			t.Fatalf("%s changed the items: %+v", name, got)
		}
	}
}

func TestOneCharactersItemsAreNeverAnothers(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	a, _ := s.Create(ctx, New{AccountID: acc, Name: "Alpha", ClassID: 2, Items: smallKit()})
	b, err := s.Create(ctx, New{AccountID: acc, Name: "Bravo", ClassID: 7, Items: []Item{item(Equipment, 2, 99, guid(1))}})
	if err != nil {
		t.Fatal(err)
	}
	if err := s.SaveInventory(ctx, b.ID, []Item{item(Equipment, 2, 100, guid(1))}); err != nil {
		t.Fatal(err)
	}
	got, _ := s.Inventory(ctx, a.ID)
	if len(got) != len(smallKit()) || got[1].ItemRecordID != 6064630214426886486 {
		t.Errorf("Alpha's items after Bravo's save: %+v", got)
	}
	if got, _ := s.Inventory(ctx, b.ID); len(got) != 1 || got[0].ItemRecordID != 100 {
		t.Errorf("Bravo's items: %+v", got)
	}
}

func withResources() []Item {
	oak := item(Resource0, 0, 6064630000000000001, guid(20))
	oak.Quantity = 17
	oak.Quality = 6
	carcass := item(Resource0, 7, 6064630000000000002, guid(21))
	carcass.Durability = 0
	ore := item(Resource1, 39, 6064630000000000003, guid(22))
	ore.Quantity = 99
	return append(smallKit(), oak, carcass, ore)
}

func verifyResourceTableSplit(t *testing.T, ctx context.Context, s *Store, charID string) {
	t.Helper()
	var inItems, inResource int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_items WHERE character_id = $1`, charID).Scan(&inItems); err != nil {
		t.Fatal(err)
	}
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_resource_items WHERE character_id = $1`, charID).Scan(&inResource); err != nil {
		t.Fatal(err)
	}
	if inItems != 4 || inResource != 3 {
		t.Errorf("%d rows in character_items and %d in character_resource_items, want 4 and 3", inItems, inResource)
	}
}

func TestResourceItemsAreStoredAndReadBackInTheOrderOfStorage(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	kit := withResources()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: kit})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	got, err := s.Inventory(ctx, c.ID)
	if err != nil {
		t.Fatal(err)
	}
	want := append([]Item{}, kit...)
	SortItems(want)
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("Inventory =\n%+v\nwant\n%+v", got, want)
	}
	order := []string{Backpack, Equipment, Equipment, Quest, Resource0, Resource0, Resource1}
	for i, o := range order {
		if got[i].Container != o {
			t.Errorf("item %d is in %s, want %s: %+v", i, got[i].Container, o, got)
		}
	}
	if got[4].Slot != 0 || got[5].Slot != 7 || got[6].Slot != 39 || got[4].Quantity != 17 || got[4].Quality != 6 {
		t.Errorf("the resource items: %+v", got[4:])
	}
	verifyResourceTableSplit(t, ctx, s, c.ID)
}

func TestSaveInventoryReplacesBothTables(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: withResources()})
	if err != nil {
		t.Fatal(err)
	}
	oak := item(Resource0, 12, 6064630000000000001, guid(20))
	oak.Quantity = 20
	oak.Quality = 6
	ore := item(Resource2, 3, 6064630000000000003, guid(22))
	ore.Quantity = 99
	reagent := item(Resource0, 0, 6064630000000000009, guid(23))
	next := append(smallKit(), oak, ore, reagent)
	if err := s.SaveInventory(ctx, c.ID, next); err != nil {
		t.Fatalf("SaveInventory: %v", err)
	}
	got, err := s.Inventory(ctx, c.ID)
	want := append([]Item{}, next...)
	SortItems(want)
	if err != nil || !reflect.DeepEqual(got, want) {
		t.Errorf("after the save:\n%+v, %v\nwant\n%+v", got, err, want)
	}
	if err := s.SaveInventory(ctx, c.ID, smallKit()); err != nil {
		t.Fatal(err)
	}
	var n int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_resource_items WHERE character_id = $1`, c.ID).Scan(&n); err != nil || n != 0 {
		t.Errorf("%d resource items remain after a save with none (%v)", n, err)
	}
	other, _ := s.Create(ctx, New{AccountID: acc, Name: "Bravo", ClassID: 2, Items: []Item{item(Backpack, 0, 1, guid(30)), item(Resource0, 0, 5, guid(31))}})
	if err := s.SaveInventory(ctx, c.ID, withResources()); err != nil {
		t.Fatal(err)
	}
	if got, _ := s.Inventory(ctx, other.ID); len(got) != 2 || got[1].Container != Resource0 || got[1].ItemRecordID != 5 {
		t.Errorf("Bravo's items after Cyb's save: %+v", got)
	}
}

// C Y B + C O A

func TestSaveInventoryRefusalsForResourceItemsChangeNothing(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: withResources()})
	if err != nil {
		t.Fatal(err)
	}
	want, _ := s.Inventory(ctx, c.ID)
	for name, items := range map[string][]Item{
		"a guid in the Backpack and in a resource container": {item(Backpack, 0, 1, guid(1)), item(Resource0, 0, 5, guid(1))},
		"a guid in two resource containers":                  {item(Resource0, 0, 5, guid(1)), item(Resource1, 0, 5, guid(1))},
		"two items in a resource slot":                       {item(Resource0, 4, 5, guid(1)), item(Resource0, 4, 5, guid(2))},
		"a resource slot out of range":                       {item(Resource1, 256, 5, guid(1))},
		"a fourth resource container":                        {item("Resource3", 0, 5, guid(1))},
	} {
		if err := s.SaveInventory(ctx, c.ID, items); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: %v, want ErrInvalid", name, err)
		}
		if got, _ := s.Inventory(ctx, c.ID); !reflect.DeepEqual(got, want) {
			t.Fatalf("%s changed the items: %+v", name, got)
		}
	}
}

func TestDeletingACharacterDeletesItsResourceItems(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	a, _ := s.Create(ctx, New{AccountID: acc, Name: "Alpha", ClassID: 2, Items: withResources()})
	if err := s.Delete(ctx, acc, a.ID); err != nil {
		t.Fatal(err)
	}
	var n int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_resource_items WHERE character_id = $1`, a.ID).Scan(&n); err != nil || n != 0 {
		t.Errorf("%d resource items of the deleted character remain (%v)", n, err)
	}
}

func TestGrantMissingKitsLeavesACharacterWithResourceItemsAlone(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "Hoarder", ClassID: 2, Items: []Item{item(Resource0, 0, 6064630000000000001, guid(40))}})
	if err != nil {
		t.Fatal(err)
	}
	captureLog(t)
	res, err := s.GrantMissingKits(ctx, &fakeKits{})
	if err != nil || res.Granted != 0 {
		t.Fatalf("GrantMissingKits = %+v, %v; a character with a resource item has items", res, err)
	}
	if got, _ := s.Inventory(ctx, c.ID); len(got) != 1 || got[0].Container != Resource0 {
		t.Errorf("the character's items after the backfill: %+v", got)
	}
}

type fakeKits struct {
	calls []int32
	onKit func(class int32)
	n     int
}

func (f *fakeKits) Kit(class int32) ([]Item, error) {
	f.calls = append(f.calls, class)
	if f.onKit != nil {
		f.onKit(class)
	}
	if class != 2 && class != 7 {
		return nil, fmt.Errorf("class %d has no starting kit", class)
	}
	f.n += 10
	return []Item{item(Equipment, 2, int64(1000+class), guid(f.n+1)), item(Equipment, 15, int64(2000+class), guid(f.n+2))}, nil
}

func captureLog(t *testing.T) *bytes.Buffer {
	t.Helper()
	var logs bytes.Buffer
	saved := slog.Default()
	slog.SetDefault(slog.New(slog.NewTextHandler(&logs, nil)))
	t.Cleanup(func() { slog.SetDefault(saved) })
	return &logs
}

func verifyBackfillInventories(t *testing.T, ctx context.Context, s *Store, grantedIDs, emptyIDs []string) {
	t.Helper()
	for _, id := range grantedIDs {
		got, _ := s.Inventory(ctx, id)
		if len(got) != 2 || got[0].Slot != 2 || got[1].Slot != 15 {
			t.Errorf("the kit of %s: %+v", id, got)
		}
	}
	for _, id := range emptyIDs {
		if got, _ := s.Inventory(ctx, id); len(got) != 0 {
			t.Errorf("a character that cannot have a kit has items: %+v", got)
		}
	}
}

func verifyBackfillLogs(t *testing.T, text string, wantSubstrings []string, forbidID string) {
	t.Helper()
	for _, want := range wantSubstrings {
		if !strings.Contains(text, want) {
			t.Errorf("the log lacks %q:\n%s", want, text)
		}
	}
	if forbidID != "" && strings.Contains(text, "character_id="+forbidID) {
		t.Errorf("the log mentions the character that had items:\n%s", text)
	}
}

func TestGrantMissingKitsGivesAKitToCharactersWithNoItemsOnlyAndIsIdempotent(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	logs := captureLog(t)
	cyb, _ := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, ClassName: "Bard", ArchetypeID: 17747})
	tank, _ := s.Create(ctx, New{AccountID: acc, Name: "Tank", ClassID: 7, ClassName: "Tank"})
	has, _ := s.Create(ctx, New{AccountID: acc, Name: "Has", ClassID: 2, Items: []Item{item(Backpack, 0, 555, guid(500))}})
	old, _ := s.Create(ctx, New{AccountID: acc, Name: "Old"})
	odd, _ := s.Create(ctx, New{AccountID: acc, Name: "Odd", ClassID: 5, ClassName: "Rogue"})

	kits := &fakeKits{}
	res, err := s.GrantMissingKits(ctx, kits)
	if err != nil {
		t.Fatal(err)
	}
	if res.Without != 4 || res.Granted != 2 || res.Items != 4 || len(res.Skipped) != 2 {
		t.Errorf("result = %+v, want 4 without items, 2 granted (4 items), 2 skipped", res)
	}
	verifyBackfillInventories(t, ctx, s, []string{cyb.ID, tank.ID}, []string{old.ID, odd.ID})
	if got, _ := s.Inventory(ctx, cyb.ID); got[0].ItemRecordID != 1002 {
		t.Errorf("cyb got %+v, want the Bard's kit (class 2)", got)
	}
	if got, _ := s.Inventory(ctx, has.ID); len(got) != 1 || got[0].ItemRecordID != 555 {
		t.Errorf("a character with items was given a kit: %+v", got)
	}
	if !reflect.DeepEqual(kits.calls, []int32{2, 7, 5}) && !reflect.DeepEqual(kits.calls, []int32{5, 2, 7}) && !reflect.DeepEqual(kits.calls, []int32{2, 5, 7}) {
		t.Errorf("kits made for classes %v", kits.calls)
	}
	wantLog := []string{"starter kit granted (backfill)", "character_id=" + cyb.ID, "name=cyb", "items=2", "no starter kit granted",
		"character_id=" + old.ID, "class_id is NULL", "character_id=" + odd.ID, "class 5 has no starting kit", "starter kit backfill",
		"characters_without_items=4", "granted=2", "skipped=2"}
	verifyBackfillLogs(t, logs.String(), wantLog, has.ID)

	before, _ := s.Inventory(ctx, cyb.ID)
	again, err := s.GrantMissingKits(ctx, kits)
	if err != nil || again.Without != 2 || again.Granted != 0 || again.Items != 0 || len(again.Skipped) != 2 {
		t.Errorf("second run = %+v, %v", again, err)
	}
	if after, _ := s.Inventory(ctx, cyb.ID); !reflect.DeepEqual(after, before) {
		t.Errorf("cyb's items changed by the second run: %+v", after)
	}
}

// ~ Cyb - C.O.A ~

func TestGrantMissingKitsNeverOverwritesItemsSavedInTheMeantime(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	captureLog(t)
	c, _ := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2})
	saved := []Item{item(Backpack, 0, 4242, guid(4242))}
	kits := &fakeKits{onKit: func(int32) {
		if err := s.SaveInventory(ctx, c.ID, saved); err != nil {
			t.Error(err)
		}
	}}
	res, err := s.GrantMissingKits(ctx, kits)
	if err != nil || res.Granted != 0 || res.Without != 1 {
		t.Errorf("result = %+v, %v; want nothing granted", res, err)
	}
	if got, _ := s.Inventory(ctx, c.ID); !reflect.DeepEqual(got, saved) {
		t.Errorf("the saved items were replaced: %+v", got)
	}
}

func TestGrantMissingKitsRefusesAKitThatBreaksTheRules(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	captureLog(t)
	c, _ := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2})
	broken := &brokenKits{}
	if _, err := s.GrantMissingKits(ctx, broken); !errors.Is(err, ErrInvalid) {
		t.Errorf("a kit with two items in a slot: %v, want ErrInvalid", err)
	}
	if got, _ := s.Inventory(ctx, c.ID); len(got) != 0 {
		t.Errorf("a broken kit was stored: %+v", got)
	}
}

type brokenKits struct{}

func (brokenKits) Kit(int32) ([]Item, error) {
	return []Item{item(Equipment, 2, 1, guid(1)), item(Equipment, 2, 2, guid(2))}, nil
}

func TestGrantKitWaitsForTheRowLockAndWritesNothingOverASave(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	captureLog(t)
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2})
	if err != nil {
		t.Fatal(err)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	if err := lockCharacter(ctx, tx, c.ID); err != nil {
		t.Fatal(err)
	}
	saved := []Item{item(Backpack, 0, 4242, guid(4242))}
	type result struct {
		granted bool
		err     error
	}
	done := make(chan result, 1)
	go func() {
		g, err := s.grantKit(ctx, c.ID, []Item{item(Equipment, 2, 1, guid(1)), item(Equipment, 15, 2, guid(2))})
		done <- result{g, err}
	}()
	select {
	case r := <-done:
		t.Fatalf("grantKit did not wait for the row lock (granted=%v err=%v)", r.granted, r.err)
	case <-time.After(400 * time.Millisecond):
	}
	if err := insertItems(ctx, tx, c.ID, saved); err != nil {
		t.Fatal(err)
	}
	if err := tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	r := <-done
	if r.err != nil || r.granted {
		t.Errorf("grantKit after the save: granted=%v err=%v, want false and no error", r.granted, r.err)
	}
	if got, _ := s.Inventory(ctx, c.ID); !reflect.DeepEqual(got, saved) {
		t.Errorf("the character has %+v, want only what was saved", got)
	}
}

// built with C O A and C Y B

func TestSaveInventoryWaitsForAnOpenKitGrant(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2})
	if err != nil {
		t.Fatal(err)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	if err := lockCharacter(ctx, tx, c.ID); err != nil {
		t.Fatal(err)
	}
	kit := []Item{item(Equipment, 2, 1, guid(1)), item(Equipment, 15, 2, guid(2))}
	if err := insertItems(ctx, tx, c.ID, kit); err != nil {
		t.Fatal(err)
	}
	saved := []Item{item(Backpack, 0, 4242, guid(4242)), item(Equipment, 2, 1, guid(1))}
	done := make(chan error, 1)
	go func() { done <- s.SaveInventory(ctx, c.ID, saved) }()
	select {
	case err := <-done:
		t.Fatalf("SaveInventory did not wait for the open kit grant (err=%v)", err)
	case <-time.After(400 * time.Millisecond):
	}
	if err := tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	got, _ := s.Inventory(ctx, c.ID)
	if len(got) != 2 || got[0].ItemRecordID != 4242 {
		t.Errorf("the character has %+v, want exactly the saved two", got)
	}
}

func verifyLockedRowSkipResult(t *testing.T, res BackfillResult, lockedID, logText string) {
	t.Helper()
	if res.Without != 2 || res.Granted != 1 || res.Items != 2 || len(res.Skipped) != 1 || !strings.Contains(res.Skipped[0], lockedID) {
		t.Errorf("result = %+v, want 2 without items, only Free granted, Locked skipped", res)
	}
	wantLog := []string{"no starter kit granted", "character_id=" + lockedID, "name=Locked", "locked by another transaction", "granted=1", "skipped=1"}
	verifyBackfillLogs(t, logText, wantLog, "")
}

func TestGrantMissingKitsSkipsACharacterWhoseRowStaysLocked(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	logs := captureLog(t)
	saved := grantLockTimeout
	grantLockTimeout = 300 * time.Millisecond
	t.Cleanup(func() { grantLockTimeout = saved })
	locked, err := s.Create(ctx, New{AccountID: acc, Name: "Locked", ClassID: 2})
	if err != nil {
		t.Fatal(err)
	}
	free, err := s.Create(ctx, New{AccountID: acc, Name: "Free", ClassID: 2})
	if err != nil {
		t.Fatal(err)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	if err := lockCharacter(ctx, tx, locked.ID); err != nil {
		t.Fatal(err)
	}
	kits := &fakeKits{}
	start := time.Now()
	res, err := s.GrantMissingKits(ctx, kits)
	if err != nil {
		t.Fatalf("a locked row stopped the backfill: %v", err)
	}
	if took := time.Since(start); took < 250*time.Millisecond || took > 4*time.Second {
		t.Errorf("the backfill took %v, want the lock timeout (300ms) and no more", took)
	}
	verifyLockedRowSkipResult(t, res, locked.ID, logs.String())
	if got, _ := s.Inventory(ctx, free.ID); len(got) != 2 {
		t.Errorf("the free character's kit: %+v", got)
	}
	if err := tx.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	again, err := s.GrantMissingKits(ctx, kits)
	if err != nil || again.Granted != 1 || len(again.Skipped) != 0 {
		t.Errorf("next run = %+v, %v; want Locked granted", again, err)
	}
	if got, _ := s.Inventory(ctx, locked.ID); len(got) != 2 {
		t.Errorf("Locked's kit: %+v", got)
	}
}
