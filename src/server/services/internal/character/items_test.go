// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"errors"
	"fmt"
	"math"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
)

var created = time.UnixMilli(1_790_000_000_000)

func item(container string, slot int32, record int64, guid string) Item {
	return Item{Container: container, Slot: slot, ItemRecordID: record, InstanceGUID: guid, Quantity: 1, Quality: 1,
		Durability: 11, CreatedAt: created}
}

func guid(n int) string { return fmt.Sprintf("%032x", n) }

func smallKit() []Item {
	return []Item{
		item(Backpack, 3, 7001, guid(1)),
		item(Equipment, 2, 6064630214426886486, guid(2)),
		item(Equipment, 15, 6064631131688402948, guid(3)),
		item(Quest, 99, 7002, guid(4)),
	}
}

// C.O.A : Cyb

type itemValidateCase struct {
	name string
	mod  func(*Item)
	want string
}

func itemValidateErrorCases() []itemValidateCase {
	return []itemValidateCase{
		{"no container", func(i *Item) { i.Container = "" }, "container"},
		{"another container", func(i *Item) { i.Container = "Bank" }, "container"},
		{"slot -1", func(i *Item) { i.Slot = -1 }, "slot -1"},
		{"Backpack slot 70", func(i *Item) { i.Slot = 70 }, "slot 70"},
		{"Equipment slot 83", func(i *Item) { i.Container, i.Slot = Equipment, 83 }, "slot 83"},
		{"Quest slot 100", func(i *Item) { i.Container, i.Slot = Quest, 100 }, "slot 100"},
		{"Resource0 slot 256", func(i *Item) { i.Container, i.Slot = Resource0, 256 }, "slot 256"},
		{"Resource2 slot -1", func(i *Item) { i.Container, i.Slot = Resource2, -1 }, "slot -1"},
		{"a fourth resource container", func(i *Item) { i.Container = "Resource3" }, "container"},
		{"record 0", func(i *Item) { i.ItemRecordID = 0 }, "record"},
		{"negative record", func(i *Item) { i.ItemRecordID = -5 }, "record"},
		{"guid in capitals", func(i *Item) { i.InstanceGUID = strings.ToUpper(guid(255)) }, "guid"},
		{"short guid", func(i *Item) { i.InstanceGUID = "abcd" }, "guid"},
		{"zero guid", func(i *Item) { i.InstanceGUID = strings.Repeat("0", 32) }, "guid"},
		{"quantity 0", func(i *Item) { i.Quantity = 0 }, "quantity"},
		{"quantity past the limit", func(i *Item) { i.Quantity = MaxQuantity + 1 }, "quantity"},
		{"quality 0", func(i *Item) { i.Quality = 0 }, "quality"},
		{"quality 201", func(i *Item) { i.Quality = MaxQuality + 1 }, "quality"},
		{"negative durability", func(i *Item) { i.Durability = -0.5 }, "durability"},
		{"NaN durability", func(i *Item) { i.Durability = float32(math.NaN()) }, "durability"},
		{"infinite durability", func(i *Item) { i.Durability = float32(math.Inf(1)) }, "durability"},
		{"no creation time", func(i *Item) { i.CreatedAt = time.Time{} }, "creation time"},
		{"negative persisted health", func(i *Item) { i.PersistedHealth = f32(-1) }, "persisted health"},
		{"NaN persisted health", func(i *Item) { i.PersistedHealth = f32(float32(math.NaN())) }, "persisted health"},
		{"persisted health past the limit", func(i *Item) { i.PersistedHealth = f32(1.5e9) }, "persisted health"},
		{"negative persisted mana", func(i *Item) { i.PersistedMana = f32(-0.5) }, "persisted mana"},
		{"infinite persisted mana", func(i *Item) { i.PersistedMana = f32(float32(math.Inf(1))) }, "persisted mana"},
		{"negative lock expiration", func(i *Item) { i.LockExpiration = -1 }, "lock expiration"},
		{"a lock past the year 9999", func(i *Item) { i.LockExpiration = 253402300800 }, "lock expiration"},
	}
}

func TestItemValidate(t *testing.T) {
	good := item(Backpack, 0, 1, guid(1))
	if err := good.Validate(); err != nil {
		t.Fatalf("a good item: %v", err)
	}
	for _, tc := range itemValidateErrorCases() {
		i := good
		tc.mod(&i)
		if err := i.Validate(); !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), tc.want) {
			t.Errorf("%s: %v, want ErrInvalid about %q", tc.name, err, tc.want)
		}
	}
	limits := Item{Container: Quest, Slot: 99, ItemRecordID: math.MaxInt64, InstanceGUID: strings.Repeat("f", 32), Quantity: MaxQuantity,
		Quality: MaxQuality, Durability: 1e9, CreatedAt: created}
	if err := limits.Validate(); err != nil {
		t.Errorf("an item at every limit: %v", err)
	}
	limits.PersistedHealth, limits.PersistedMana, limits.LockExpiration = f32(1e9), f32(0), 253402300799
	if err := limits.Validate(); err != nil {
		t.Errorf("a mount item at every limit: %v", err)
	}
}

func f32(v float32) *float32 { return &v }

func verifyMountPersistedReadback(t *testing.T, got []Item) {
	t.Helper()
	if got[1].PersistedHealth == nil || *got[1].PersistedHealth != 3572 || *got[1].PersistedMana != 1 || got[1].LockExpiration != 1770131735 {
		t.Errorf("the bird persisted state read back: %+v", got[1])
	}
	if got[2].PersistedHealth == nil || *got[2].PersistedHealth != 0 || got[3].PersistedHealth != nil || got[3].PersistedMana != nil {
		t.Errorf("the dead/onlyLock persisted state read back: %+v", got[2:4])
	}
	if got[3].LockExpiration != 1790000123 || got[4].PersistedHealth != nil || got[4].LockExpiration != 0 {
		t.Errorf("the lock/plain persisted state read back: %+v", got[3:5])
	}
}

func verifyMountSaveUpdateAndRefusal(t *testing.T, ctx context.Context, s *Store, charID string, bird Item) {
	t.Helper()
	if err := s.SaveInventory(ctx, charID, []Item{bird}); err != nil {
		t.Fatal(err)
	}
	if got, _ := s.Inventory(ctx, charID); len(got) != 1 || *got[0].PersistedHealth != 1200.5 || *got[0].PersistedMana != 1 || got[0].LockExpiration != 0 {
		t.Errorf("after the second save: %+v", got)
	}
	bad := bird
	bad.PersistedHealth = f32(-3)
	if err := s.SaveInventory(ctx, charID, []Item{bad}); !errors.Is(err, ErrInvalid) {
		t.Errorf("a negative persisted health: %v, want ErrInvalid", err)
	}
	if got, _ := s.Inventory(ctx, charID); len(got) != 1 || *got[0].PersistedHealth != 1200.5 {
		t.Errorf("a refused save changed the items: %+v", got)
	}
}

func TestSaveInventoryKeepsAMountItemsPersistedState(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "rider", ClassID: 7, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	bird := item(Backpack, 5, 6064631121016127508, guid(20))
	bird.PersistedHealth, bird.PersistedMana, bird.LockExpiration = f32(3572), f32(1), 1770131735
	dead := item(Backpack, 6, 6064634325298708480, guid(21))
	dead.PersistedHealth, dead.PersistedMana, dead.LockExpiration = f32(0), f32(1), 1790003600
	onlyLock := item(Backpack, 7, 6064634099174146054, guid(22))
	onlyLock.LockExpiration = 1790000123
	plain := item(Backpack, 8, 7001, guid(23))
	set := []Item{item(Backpack, 3, 7001, guid(1)), bird, dead, onlyLock, plain}
	if err := s.SaveInventory(ctx, c.ID, set); err != nil {
		t.Fatalf("SaveInventory: %v", err)
	}
	got, err := s.Inventory(ctx, c.ID)
	if err != nil || !reflect.DeepEqual(got, set) {
		t.Fatalf("after the save: %+v, %v; want %+v", got, err, set)
	}
	verifyMountPersistedReadback(t, got)
	bird.PersistedHealth, bird.LockExpiration = f32(1200.5), 0
	verifyMountSaveUpdateAndRefusal(t, ctx, s, c.ID, bird)
	other, err := s.Create(ctx, New{AccountID: acc, Name: "rider2", ClassID: 7, Items: []Item{bird}})
	if err != nil {
		t.Fatalf("Create with a mount item: %v", err)
	}
	if got, _ := s.Inventory(ctx, other.ID); len(got) != 1 || *got[0].PersistedHealth != 1200.5 {
		t.Errorf("the created character's mount item: %+v", got)
	}
}

// C:Y:B + CoA

func fillAllItemSlots() []Item {
	var all []Item
	n := 0
	for _, c := range []string{Backpack, Equipment, Quest, Resource0, Resource1, Resource2} {
		for s := int32(0); s < SlotCount[c]; s++ {
			n++
			all = append(all, item(c, s, 1, guid(n)))
		}
	}
	return all
}

func TestValidateItemsRefusesDuplicatesAndTooMany(t *testing.T) {
	if err := ValidateItems(smallKit()); err != nil {
		t.Fatal(err)
	}
	slot := append(smallKit(), item(Equipment, 2, 9, guid(50)))
	if err := ValidateItems(slot); !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "two items in Equipment slot 2") {
		t.Errorf("two items in a slot: %v", err)
	}
	same := append(smallKit(), item(Backpack, 4, 9, guid(2)))
	if err := ValidateItems(same); !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "two items with instance guid") {
		t.Errorf("two items with a guid: %v", err)
	}
	if err := ValidateItems([]Item{item(Backpack, 5, 1, guid(1)), item(Equipment, 5, 1, guid(2)), item(Quest, 5, 1, guid(3))}); err != nil {
		t.Errorf("slot 5 of three storages: %v", err)
	}
	all := fillAllItemSlots()
	if len(all) != MaxItems {
		t.Fatalf("%d slots, MaxItems %d", len(all), MaxItems)
	}
	if err := ValidateItems(all); err != nil {
		t.Errorf("every slot filled: %v", err)
	}
	if err := ValidateItems(append(all, item(Backpack, 0, 1, guid(999)))); !errors.Is(err, ErrInvalid) {
		t.Errorf("one item more than every slot: %v", err)
	}
}

func verifyCreatedChestItem(t *testing.T, it Item) {
	t.Helper()
	if it.ItemRecordID != 6064630214426886486 || it.InstanceGUID != guid(2) || it.Quantity != 1 {
		t.Errorf("the chest identity: %+v", it)
	}
	if it.Quality != 1 || it.Durability != 11 || !it.CreatedAt.Equal(created) {
		t.Errorf("the chest state: %+v", it)
	}
}

func TestCreateStoresItemsWithTheCharacterAndReadsThemBackInOrder(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	kit := smallKit()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, ClassName: "Bard", ArchetypeID: 17747, Items: kit})
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
		t.Errorf("Inventory =\n%+v\nwant\n%+v", got, want)
	}
	if got[0].Container != Backpack || got[1].Container != Equipment || got[1].Slot != 2 || got[2].Slot != 15 || got[3].Container != Quest {
		t.Errorf("not in order of storage and slot: %+v", got)
	}
	verifyCreatedChestItem(t, got[1])
}

func TestCreateWithoutItemsHasNone(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "Bare", ClassID: 2})
	if err != nil {
		t.Fatal(err)
	}
	got, err := s.Inventory(ctx, c.ID)
	if err != nil || len(got) != 0 {
		t.Errorf("Inventory = %+v, %v; want none and no error", got, err)
	}
}

func TestACharacterThatCannotBeCreatedHasNoItemsAndTheOtherKeepsItsOwn(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	first, err := s.Create(ctx, New{AccountID: acc, Name: "First", ClassID: 2, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.Create(ctx, New{AccountID: acc, Name: "first", ClassID: 2, Items: smallKit()}); !errors.Is(err, ErrNameTaken) {
		t.Errorf("name taken: %v", err)
	}
	bad := append(smallKit(), item(Backpack, 3, 1, guid(77)))
	if _, err := s.Create(ctx, New{AccountID: acc, Name: "Second", ClassID: 2, Items: bad}); !errors.Is(err, ErrInvalid) {
		t.Errorf("two items in a slot: %v", err)
	}
	if list, err := s.List(ctx, acc); err != nil || len(list) != 1 || list[0].ID != first.ID {
		t.Errorf("characters = %+v, %v; want only the first", list, err)
	}
	var n int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_items`).Scan(&n); err != nil || n != len(smallKit()) {
		t.Errorf("%d items in the table, want the first character's %d (%v)", n, len(smallKit()), err)
	}
	other := New{AccountID: acc, Name: "Third", ClassID: 2, Items: []Item{item(Backpack, 0, 1, guid(1))}}
	if _, err := s.Create(ctx, other); err != nil {
		t.Errorf("the same guid under another character is allowed: %v", err)
	}
}

func TestInventoryOfUnknownAndMalformedIDsIsNotFound(t *testing.T) {
	s, _ := setup(t)
	ctx := context.Background()
	for _, id := range []string{"ffffffffffffffffffffffffffffffff", "", "not-an-id", "BBEC5C86EB101FE5F38C87F0FA22E7D0", "ab\x00cd"} {
		if _, err := s.Inventory(ctx, id); !errors.Is(err, ErrNotFound) {
			t.Errorf("Inventory(%q) = %v, want ErrNotFound", id, err)
		}
		if err := s.SaveInventory(ctx, id, smallKit()); !errors.Is(err, ErrNotFound) {
			t.Errorf("SaveInventory(%q) = %v, want ErrNotFound", id, err)
		}
	}
}

// built by CoA with C_Y_B

func TestDeletingACharacterDeletesItsItems(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	a, _ := s.Create(ctx, New{AccountID: acc, Name: "Alpha", ClassID: 2, Items: smallKit()})
	b, _ := s.Create(ctx, New{AccountID: acc, Name: "Bravo", ClassID: 2, Items: []Item{item(Backpack, 0, 1, guid(9))}})
	if err := s.Delete(ctx, acc, a.ID); err != nil {
		t.Fatal(err)
	}
	var n int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_items WHERE character_id = $1`, a.ID).Scan(&n); err != nil || n != 0 {
		t.Errorf("%d items of the deleted character remain (%v)", n, err)
	}
	if got, _ := s.Inventory(ctx, b.ID); len(got) != 1 {
		t.Errorf("the other character's items: %+v", got)
	}
}

func TestSaveInventoryDiffValidation(t *testing.T) {
	s := &Store{}
	ctx := context.Background()
	validID := strings.Repeat("a", 32)
	goodItem := item(Backpack, 2, 7001, guid(10))
	negQty := goodItem
	negQty.Quantity = -1

	cases := []struct {
		name string
		diff InventoryDiffSave
		want error
	}{
		{"invalid character id", InventoryDiffSave{CharacterID: "bad-id", FencingToken: 1, Upserts: []Item{goodItem}}, ErrInvalid},
		{"negative fencing token", InventoryDiffSave{CharacterID: validID, FencingToken: -1, Upserts: []Item{goodItem}}, ErrInvalid},
		{"upsert unknown container", InventoryDiffSave{CharacterID: validID, FencingToken: 1, Upserts: []Item{item("Vault", 0, 7001, guid(11))}}, ErrInvalid},
		{"upsert slot out of range", InventoryDiffSave{CharacterID: validID, FencingToken: 1, Upserts: []Item{item(Backpack, 70, 7001, guid(12))}}, ErrInvalid},
		{"upsert negative quantity", InventoryDiffSave{CharacterID: validID, FencingToken: 1, Upserts: []Item{negQty}}, ErrInvalid},
		{"delete unknown container", InventoryDiffSave{CharacterID: validID, FencingToken: 1, Deletes: []SlotKey{{Container: "Vault", Slot: 0}}}, ErrInvalid},
		{"delete slot out of range", InventoryDiffSave{CharacterID: validID, FencingToken: 1, Deletes: []SlotKey{{Container: Equipment, Slot: 83}}}, ErrInvalid},
		{"delete negative slot", InventoryDiffSave{CharacterID: validID, FencingToken: 1, Deletes: []SlotKey{{Container: Backpack, Slot: -1}}}, ErrInvalid},
		{"duplicate delete slot", InventoryDiffSave{CharacterID: validID, FencingToken: 1, Deletes: []SlotKey{{Container: Backpack, Slot: 1}, {Container: Backpack, Slot: 1}}}, ErrInvalid},
		{"upsert and delete same slot", InventoryDiffSave{CharacterID: validID, FencingToken: 1, Upserts: []Item{goodItem}, Deletes: []SlotKey{{Container: Backpack, Slot: 2}}}, ErrInvalid},
		{"mutation nil item", InventoryDiffSave{CharacterID: validID, FencingToken: 1, Mutations: []SlotMutation{{Container: Backpack, Slot: 4, Deleted: false}}}, ErrInvalid},
		{"invalid wallet entry", InventoryDiffSave{CharacterID: validID, FencingToken: 1, Upserts: []Item{goodItem}, Wallet: []WalletEntry{{CurrencyGuid: 0, Amount: 10}}}, ErrInvalid},
	}
	for _, tc := range cases {
		if err := s.SaveInventoryDiff(ctx, tc.diff); !errors.Is(err, tc.want) {
			t.Errorf("%s: got %v, want %v", tc.name, err, tc.want)
		}
	}
}

type fakeFenceRow struct {
	ok  bool
	err error
}

func (r fakeFenceRow) Scan(dest ...any) error {
	if r.err != nil {
		return r.err
	}
	*(dest[0].(*bool)) = r.ok
	return nil
}

type fakeDiffTx struct {
	tokens  map[string]int64
	queries []string
}

func (f *fakeDiffTx) QueryRow(_ context.Context, sql string, args ...any) pgx.Row {
	f.queries = append(f.queries, sql)
	charID, token := args[0].(string), args[1].(int64)
	cur, exists := f.tokens[charID]
	if !exists {
		return fakeFenceRow{err: pgx.ErrNoRows}
	}
	if token < cur {
		return fakeFenceRow{ok: false}
	}
	f.tokens[charID] = token
	return fakeFenceRow{ok: true}
}

func (f *fakeDiffTx) Exec(_ context.Context, sql string, _ ...any) (pgconn.CommandTag, error) {
	f.queries = append(f.queries, sql)
	return pgconn.NewCommandTag("1"), nil
}

func TestSaveInventoryDiffFencingAndStatementBudget(t *testing.T) {
	ctx := context.Background()
	charID := strings.Repeat("b", 32)
	tx := &fakeDiffTx{tokens: map[string]int64{charID: 10}}

	diff := InventoryDiffSave{
		CharacterID:  charID,
		FencingToken: 11,
		Upserts:      []Item{item(Backpack, 0, 7001, guid(1))},
		Deletes:      []SlotKey{{Container: Backpack, Slot: 3}},
	}
	upserts, deletes, err := validateInventoryDiff(diff)
	if err != nil {
		t.Fatalf("validateInventoryDiff: %v", err)
	}
	if err := applyInventoryDiffTx(ctx, tx, diff, upserts, deletes); err != nil {
		t.Fatalf("applyInventoryDiffTx(token=11): %v", err)
	}
	// BEGIN (1) + fencing CTE (1) + DELETE unnest (1) + INSERT unnest (1) + COMMIT (1) = 5 statements <= 7.
	totalStatements := len(tx.queries) + 2
	if totalStatements > 7 {
		t.Errorf("2-slot dirty tick statement count = %d, want <= 7", totalStatements)
	}
	stale := diff
	stale.FencingToken = 10
	if err := applyInventoryDiffTx(ctx, tx, stale, upserts, deletes); !errors.Is(err, ErrStaleFencingToken) {
		t.Errorf("stale token 10 after 11: got %v, want ErrStaleFencingToken", err)
	}
	missing := diff
	missing.CharacterID = strings.Repeat("c", 32)
	if err := applyInventoryDiffTx(ctx, tx, missing, upserts, deletes); !errors.Is(err, ErrNotFound) {
		t.Errorf("unknown character: got %v, want ErrNotFound", err)
	}
}

func TestSaveInventoryDiffLiveDB(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	ch, err := s.Create(ctx, New{AccountID: acc, Name: "DiffHero", ClassID: 2, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	updatedSlot2 := item(Equipment, 2, 6064630214426886486, guid(2))
	updatedSlot2.Quantity = 5
	newResource := item(Resource0, 4, 7005, guid(55))
	diff := InventoryDiffSave{
		CharacterID:  ch.ID,
		FencingToken: 42,
		Upserts:      []Item{updatedSlot2, newResource},
		Deletes:      []SlotKey{{Container: Backpack, Slot: 3}},
	}
	if err := s.SaveInventoryDiff(ctx, diff); err != nil {
		t.Fatalf("SaveInventoryDiff(token=42): %v", err)
	}
	stale := InventoryDiffSave{
		CharacterID:  ch.ID,
		FencingToken: 41,
		Deletes:      []SlotKey{{Container: Quest, Slot: 99}},
	}
	if err := s.SaveInventoryDiff(ctx, stale); !errors.Is(err, ErrStaleFencingToken) {
		t.Fatalf("stale SaveInventoryDiff(token=41): got %v, want ErrStaleFencingToken", err)
	}
	got, err := s.Inventory(ctx, ch.ID)
	if err != nil {
		t.Fatal(err)
	}
	want := []Item{updatedSlot2, item(Equipment, 15, 6064631131688402948, guid(3)), item(Quest, 99, 7002, guid(4)), newResource}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("Inventory after diff save:\n got %+v\nwant %+v", got, want)
	}
}

func TestGrantMissingKitsLockedRowWaitsAtLeastTimeout(t *testing.T) {
	ctx := context.Background()
	s, acc := setup(t)
	saved := grantLockTimeout
	grantLockTimeout = 300 * time.Millisecond
	t.Cleanup(func() { grantLockTimeout = saved })
	ch, err := s.Create(ctx, New{AccountID: acc, Name: "LockedKitWait", ClassID: 2})
	if err != nil {
		t.Fatal(err)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	if err := lockCharacter(ctx, tx, ch.ID); err != nil {
		t.Fatal(err)
	}
	kits := &fakeKits{}
	t0 := time.Now()
	res, err := s.GrantMissingKits(ctx, kits)
	took := time.Since(t0)
	if err != nil || res.Granted != 0 || len(res.Skipped) != 1 {
		t.Fatalf("GrantMissingKits with a locked character = (%+v, %v), want 0 granted and 1 skipped", res, err)
	}
	if took < 250*time.Millisecond {
		t.Errorf("the locked row was skipped after %v, want at least 250ms", took)
	}
	if err := tx.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	if again, err := s.GrantMissingKits(ctx, kits); err != nil || again.Granted != 1 || len(again.Skipped) != 0 {
		t.Fatalf("GrantMissingKits on the next start = (%+v, %v), want 1 granted and 0 skipped", again, err)
	}
}

