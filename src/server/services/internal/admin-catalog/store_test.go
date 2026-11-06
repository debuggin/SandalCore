// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admincatalog

import (
	"context"
	"database/sql"
	"os"
	"path/filepath"
	"testing"
)

func TestCatalogLoadContentDBAndSearch(t *testing.T) {
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "content.db")
	db, err := sql.Open("sqlite", dbPath)
	if err != nil {
		t.Fatalf("sql.Open: %v", err)
	}
	ddl := `
	CREATE TABLE equip_slot (slot INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);
	INSERT INTO equip_slot (slot, name) VALUES (1, 'Primary'), (2, 'Helmet');
	CREATE TABLE quality_curve (guid INTEGER NOT NULL, ord INTEGER NOT NULL, x REAL NOT NULL, y REAL NOT NULL, PRIMARY KEY (guid, ord));
	INSERT INTO quality_curve (guid, ord, x, y) VALUES (100, 1, 0, 50), (100, 5, 50, 100);
	CREATE TABLE item (
		guid INTEGER PRIMARY KEY,
		name TEXT NOT NULL,
		level INTEGER NOT NULL,
		max_stack_size INTEGER NOT NULL,
		quality_curve_guid INTEGER NOT NULL,
		display_name TEXT NOT NULL DEFAULT ''
	);
	CREATE TABLE item_equip_slot (item_guid INTEGER NOT NULL, ord INTEGER NOT NULL, slot INTEGER NOT NULL, PRIMARY KEY (item_guid, ord));
	INSERT INTO item (guid, name, level, max_stack_size, quality_curve_guid, display_name) VALUES
		(6064629000001, 'Item_Weapon_2H_Sword_Radiant', 25, 1, 100, '2HandedSword'),
		(6064629000002, 'Item_Resource_Coconut_Wood', 10, 50, 0, 'Coconut Wood');
	INSERT INTO item_equip_slot (item_guid, ord, slot) VALUES (6064629000001, 0, 1);

	CREATE TABLE npc_asset (guid INTEGER PRIMARY KEY, name TEXT NOT NULL, archetype_path TEXT NOT NULL);
	CREATE TABLE npc_asset_set_member (set_guid INTEGER NOT NULL, ord INTEGER NOT NULL, asset_guid INTEGER NOT NULL, weight REAL NOT NULL, PRIMARY KEY (set_guid, ord));
	CREATE TABLE npc_instance_asset_set (instance_guid INTEGER NOT NULL, ord INTEGER NOT NULL, set_guid INTEGER NOT NULL, PRIMARY KEY (instance_guid, ord));
	CREATE TABLE npc_instance (guid INTEGER PRIMARY KEY, name TEXT NOT NULL, level_min INTEGER NOT NULL, level_max INTEGER NOT NULL);
	INSERT INTO npc_asset (guid, name, archetype_path) VALUES
		(6064631997858578450, 'PopAsset_Goblin_Shaman', '/Game/DesignAssets/Population/NPC/Goblin/Goblin_Shaman.Default__Goblin_Shaman_C'),
		(6064632000000000001, 'PopAsset_Zombie_Walker', '/Game/DesignAssets/Population/NPC/Undead/Zombie_Walker.Default__Zombie_Walker_C');
	INSERT INTO npc_asset_set_member VALUES (500, 0, 6064631997858578450, 1.0), (501, 0, 6064632000000000001, 1.0);
	INSERT INTO npc_instance_asset_set VALUES (700, 0, 500), (701, 0, 501);
	INSERT INTO npc_instance VALUES (700, 'Inst_Goblin', 5, 12), (701, 'Inst_Zombie', 15, 20);

	CREATE TABLE city_node (
		rank INTEGER NOT NULL UNIQUE, guid INTEGER PRIMARY KEY, name TEXT NOT NULL,
		node_name TEXT NOT NULL, node_type TEXT NOT NULL, x REAL NOT NULL, y REAL NOT NULL,
		z REAL NOT NULL, yaw REAL NOT NULL, radius REAL NOT NULL, levelable INTEGER NOT NULL
	);
	INSERT INTO city_node VALUES (0, 108646108518008, 'Verra_RVR_Winstead', 'Winstead', 'Economic', 0, 0, 0, 0, 50000, 1);
	`
	if _, err := db.Exec(ddl); err != nil {
		t.Fatalf("exec ddl: %v", err)
	}
	_ = db.Close()

	itemTSV := "table\tkey\ttext\n/Game/UI_Kit/Items.Items\t2HandedSword\tRadiant Greatsword\n"
	if err := os.WriteFile(filepath.Join(dir, "item_names.tsv"), []byte(itemTSV), 0o600); err != nil {
		t.Fatalf("write item_names.tsv: %v", err)
	}
	npcTSV := "kind\tsource\tmember\ttext\nasset\t6064631997858578450\t0\tGrem the Goblin\ninstance\t701\t0\tGravewalker Zombie\n"
	if err := os.WriteFile(filepath.Join(dir, "npc_names.tsv"), []byte(npcTSV), 0o600); err != nil {
		t.Fatalf("write npc_names.tsv: %v", err)
	}

	cat, err := NewCatalogFromContentDB(dbPath)
	if err != nil {
		t.Fatalf("NewCatalogFromContentDB: %v", err)
	}
	defer func() { _ = cat.Close() }()

	items := cat.SearchItems(context.Background(), "Radiant", 10)
	if len(items) != 1 || items[0].DisplayName != "Radiant Greatsword" || items[0].EquipSlot != "Primary" || items[0].RarityMin != 1 || items[0].RarityMax != 5 {
		t.Fatalf("unexpected SearchItems result: %+v", items)
	}
	for _, it := range cat.SearchItems(context.Background(), "", 100) {
		if it.Name == "item_0001" {
			t.Fatalf("unexpected fake item_0001 row when content.db is loaded")
		}
	}

	npcs := cat.SearchNpcs(context.Background(), "Grem", 10)
	if len(npcs) != 1 || npcs[0].DisplayName != "Grem the Goblin" || npcs[0].MinLevel != 5 || npcs[0].MaxLevel != 12 {
		t.Fatalf("unexpected SearchNpcs(Grem): %+v", npcs)
	}
	zombies := cat.SearchNpcs(context.Background(), "Gravewalker", 10)
	if len(zombies) != 1 || zombies[0].BlueprintLeaf != "Zombie_Walker_C" {
		t.Fatalf("unexpected SearchNpcs(Gravewalker): %+v", zombies)
	}

	places, total := cat.SearchPlacesFiltered(context.Background(), PlaceFilter{
		Query:      "Winstead",
		NodeLevels: map[string]int{"winstead": 4},
	})
	if total != 1 || len(places) != 1 || places[0].Level != 4 || places[0].StageName != "Town" || places[0].X == 0 {
		t.Fatalf("unexpected SearchPlaces(Winstead): %+v", places)
	}
}
