// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"path/filepath"
	"testing"
)

func openSeededDB(t *testing.T, fn func(*testing.T, *sql.DB)) *DB {
	t.Helper()
	path := filepath.Join(t.TempDir(), "content.db")
	raw, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	exec(t, raw, `PRAGMA synchronous = OFF; PRAGMA journal_mode = MEMORY`)
	if err := Create(raw); err != nil {
		t.Fatal(err)
	}
	exec(t, raw, `INSERT INTO meta (key, value) VALUES (?, ?), (?, ?)`,
		MetaSchemaVersion, "1", MetaRecordEncoding, EncodingJSON)
	if fn != nil {
		fn(t, raw)
	}
	if err := raw.Close(); err != nil {
		t.Fatal(err)
	}
	db, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	return db
}

func TestCombatReader(t *testing.T) {
	db := openSeededDB(t, func(t *testing.T, raw *sql.DB) {
		exec(t, raw, `INSERT INTO combat_profile (key, stat_guid) VALUES ('PowerMult', 101), ('Physical.Damage', 102)`)
		exec(t, raw, `INSERT INTO combat_element (tag, damage, damage_mult, damage_add_mult, mitigation)
			VALUES ('Element.Physical.Slash', 201, 202, 203, 204)`)
		exec(t, raw, `INSERT INTO relationship (guid, name, permissions)
			VALUES (301, 'Hostile', 1)`)
		exec(t, raw, `INSERT INTO alignment_relation (alignment, other, relationship)
			VALUES (401, 402, 301)`)
		exec(t, raw, `INSERT INTO combat_config (key, value, source)
			VALUES ('global_damage_multiplier', 1.5, 'design')`)
	})

	cfg, err := db.Combat()
	if err != nil {
		t.Fatalf("Combat: %v", err)
	}
	if cfg.Profile["PowerMult"] != 101 || cfg.Profile["Physical.Damage"] != 102 {
		t.Fatalf("unexpected profile: %+v", cfg.Profile)
	}
	if len(cfg.Elements) != 1 || cfg.Elements[0].Tag != "Element.Physical.Slash" || cfg.Elements[0].Mitigation != 204 {
		t.Fatalf("unexpected elements: %+v", cfg.Elements)
	}
	if cfg.Permissions[301] != PermissionHostile {
		t.Fatalf("unexpected permissions: %+v", cfg.Permissions)
	}
	if cfg.Relation[401][402] != 301 {
		t.Fatalf("unexpected relation: %+v", cfg.Relation)
	}
	if cfg.Config[CombatGlobalDamageMult] != 1.5 {
		t.Fatalf("unexpected config: %+v", cfg.Config)
	}
}

func TestFoliageAndGatherReaders(t *testing.T) {
	db := openSeededDB(t, func(t *testing.T, raw *sql.DB) {
		exec(t, raw, `INSERT INTO foliage_def VALUES (10, 'Tree_Oak_Large_1', 60.0, 0, 1, 2, 2, 0, 2, 11, 12, 13)`)
		exec(t, raw, `INSERT INTO foliage_state VALUES (13, 'AdultOak', 120.0, 0, 20, -1.0, -1.0, 45.0)`)
		exec(t, raw, `INSERT INTO gather_interaction VALUES (20, 'ChopOak', 3, 60, 100, 0, 29, 4.5, 1, 2, 70, 30, 250.0, 50.0, 1)`)
		exec(t, raw, `INSERT INTO gather_interaction_def VALUES (30, 'ChopOakDef', 20, 250.0, 50.0, 500.0, 1)`)
		exec(t, raw, `INSERT INTO gather_def VALUES (50, 'Tree_Oak_Large_2', 10, 60.0, 0.0, 0, 1, 'Adult', 30, 250.0, 50.0, 500.0, 4.5, 1, 1, 0, 1, 20, 60, 29, 100, 0, 4.5, 1, 2, 70)`)
		exec(t, raw, `INSERT INTO profession VALUES (60, 'Lumberjacking', 'Gathering', 90, 91)`)
		exec(t, raw, `INSERT INTO certification_level VALUES (100, 'Novice', 0, 1, 10, 1, 5)`)
	})

	fdefs, err := db.FoliageDefs()
	if err != nil || len(fdefs) != 1 || fdefs[0].Name != "Tree_Oak_Large_1" || fdefs[0].StateGuids[2] != 13 {
		t.Fatalf("FoliageDefs: %+v (%v)", fdefs, err)
	}
	fstates, err := db.FoliageStates()
	if err != nil || len(fstates) != 1 || fstates[0].InteractionGuid != 20 || fstates[0].CollisionRadius != 45.0 {
		t.Fatalf("FoliageStates: %+v (%v)", fstates, err)
	}
	gints, err := db.GatherInteractions()
	if err != nil || len(gints) != 1 || gints[0].BaseDuration != 4.5 || !gints[0].AllowInCombat {
		t.Fatalf("GatherInteractions: %+v (%v)", gints, err)
	}
	gidefs, err := db.GatherInteractionDefs()
	if err != nil || len(gidefs) != 1 || gidefs[0].GatherGuid != 20 || !gidefs[0].AllowInCombat {
		t.Fatalf("GatherInteractionDefs: %+v (%v)", gidefs, err)
	}
	gdefs, err := db.GatherDefs()
	if err != nil || len(gdefs) != 1 || gdefs[0].EmptyDuration != 60.0 || !gdefs[0].SingleUse {
		t.Fatalf("GatherDefs: %+v (%v)", gdefs, err)
	}
	profs, err := db.Professions()
	if err != nil || len(profs) != 1 || profs[0].Name != "Lumberjacking" {
		t.Fatalf("Professions: %+v (%v)", profs, err)
	}
	certs, err := db.CertificationLevels()
	if err != nil || len(certs) != 1 || certs[0].MaxLevel != 10 || !certs[0].HasMaxAllowable {
		t.Fatalf("CertificationLevels: %+v (%v)", certs, err)
	}
}

func TestItemAndResourceBagReaders(t *testing.T) {
	db := openSeededDB(t, func(t *testing.T, raw *sql.DB) {
		exec(t, raw, `INSERT INTO equip_slot (slot, name) VALUES (0, 'Helmet'), (12, 'PrimaryWeapon')`)
		exec(t, raw, `INSERT INTO item (guid, name, level, can_equip, occupies_all_equip_slots, default_quality, max_stack_size,
			stat_block_guid, appearance_guid, max_durability, quality_curve_guid, inv_w, inv_h, resource, display_name, stack_stats)
			VALUES (500, 'Gear_Sword_1H', 5, 1, 0, 100, 1, 0, 0, 50.0, 0, 1, 2, 0, 'Iron Sword', 0)`)
		exec(t, raw, `INSERT INTO item_equip_slot (item_guid, ord, slot) VALUES (500, 0, 12)`)
		exec(t, raw, `INSERT INTO starting_equipment (class_guid, class_name, container, ord, slot, item_guid, variation_guid, quantity)
			VALUES (7, 'Bard', 'Equipment', 0, 12, 500, 0, 1)`)
		exec(t, raw, `INSERT INTO resource_bag (item_guid, def_guid, def_name, columns, rows, custom)
			VALUES (700, 710, 'SmallBagDef', 2, 2, 1), (701, 711, 'PlainBagDef', 2, 2, 0)`)
		exec(t, raw, `INSERT INTO resource_bag_cell (def_guid, col, row, value)
			VALUES (710, 0, 0, -1), (710, 1, 1, -2), (711, 0, 0, -2)`)
		exec(t, raw, `INSERT INTO item_weapon (item_guid, attack_ability_guid, skill_line_guid, skill_line_name)
			VALUES (500, 910, 900, 'SwordLine')`)
	})

	verifyItemsAndSlots(t, db)
	verifyResourceBagsAndWeapons(t, db)
}

func verifyItemsAndSlots(t *testing.T, db *DB) {
	t.Helper()
	it, err := db.Item(500)
	if err != nil || it.DisplayName != "Iron Sword" || len(it.EquipSlots) != 1 || it.EquipSlots[0] != 12 {
		t.Fatalf("Item(500): %+v (%v)", it, err)
	}
	if _, err := db.Item(999999); err == nil {
		t.Fatal("expected error for missing item")
	}
	byName, err := db.ItemByName("Gear_Sword_1H")
	if err != nil || byName.Guid != 500 {
		t.Fatalf("ItemByName: %+v (%v)", byName, err)
	}
	if _, err := db.ItemByName("MissingItem"); !IsNoRow(err) {
		t.Fatalf("expected IsNoRow for missing item name, got %v", err)
	}
	liked, err := db.ItemsNamedLike("Gear|_Sword%")
	if err != nil || len(liked) != 1 || liked[0].Guid != 500 {
		t.Fatalf("ItemsNamedLike: %+v (%v)", liked, err)
	}
	kit, err := db.StartingKit("Bard")
	if err != nil || len(kit) != 1 || kit[0].ItemGuid != 500 {
		t.Fatalf("StartingKit(Bard): %+v (%v)", kit, err)
	}
	if _, err := db.StartingKit("UnknownClass"); err == nil {
		t.Fatal("expected error for unknown class StartingKit")
	}
	slots, err := db.EquipSlotNames()
	if err != nil || len(slots) != 2 || slots[0] != "Helmet" || slots[12] != "PrimaryWeapon" {
		t.Fatalf("EquipSlotNames: %+v (%v)", slots, err)
	}
}

func verifyResourceBagsAndWeapons(t *testing.T, db *DB) {
	t.Helper()
	bag, err := db.ResourceBag(700)
	if err != nil || bag.DefName != "SmallBagDef" || !bag.Custom || bag.Blocked() != 1 {
		t.Fatalf("ResourceBag(700): %+v (%v)", bag, err)
	}
	plain, err := db.ResourceBag(701)
	if err != nil || plain.Custom || plain.Blocked() != 0 {
		t.Fatalf("ResourceBag(701): %+v (%v)", plain, err)
	}
	if _, err := db.ResourceBag(999999); err == nil {
		t.Fatal("expected error for missing ResourceBag")
	}
	allBags, err := db.ResourceBags()
	if err != nil || len(allBags) != 2 {
		t.Fatalf("ResourceBags: %+v (%v)", allBags, err)
	}
	wp, err := db.ItemWeapon(500)
	if err != nil || wp.SkillLineName != "SwordLine" || wp.AttackAbilityGuid != 910 {
		t.Fatalf("ItemWeapon(500): %+v (%v)", wp, err)
	}
	if _, err := db.ItemWeapon(999999); !IsNoRow(err) {
		t.Fatalf("expected IsNoRow for missing ItemWeapon, got %v", err)
	}
}

func seedProgressionAndTalents(t *testing.T, raw *sql.DB) {
	t.Helper()
	exec(t, raw, `INSERT INTO item_use (item_guid, activation, consume_on_use, cooldown, disabled_in_combat)
		VALUES (500, 'ActivateActionResponses', 1, 10.0, 0)`)
	exec(t, raw, `INSERT INTO item_action (item_guid, ord, action_guid) VALUES (500, 0, 600)`)
	exec(t, raw, `INSERT INTO action_response (guid, name, response_type, class_guid, class_name, remove_up_to_quantity)
		VALUES (600, 'DrinkPotion', 'GiveItem', 0, '', 0)`)
	exec(t, raw, `INSERT INTO action_reward (action_guid, ord, reward_table_guid) VALUES (600, 0, 610)`)
	exec(t, raw, `INSERT INTO reward_table (guid, name, predicate, level_below, modelled)
		VALUES (610, 'PotionRewards', '', 0, 1)`)
	exec(t, raw, `INSERT INTO reward_item (table_guid, ord, item_guid, variation_guid, quantity)
		VALUES (610, 0, 501, 0, 2)`)
	exec(t, raw, `INSERT INTO reward_xp (table_guid, ord, experience_type, target_level, margin, curve_guid, mult_stat_guid, expression, expr)
		VALUES (610, 0, 'Adventuring', 5, 1.0, 710, 711, '250', 0)`)
	exec(t, raw, `INSERT INTO leveling_type (guid, name, experience_type, level_cap, uses_debt, xp_mult_stat_guid)
		VALUES (700, 'Adventuring', 'Adventuring', 50, 1, 701)`)
	exec(t, raw, `INSERT INTO leveling_curve (type_guid, level, xp)
		VALUES (700, 1, 0), (700, 2, 1000)`)
	exec(t, raw, `INSERT INTO skill_auto_grant (tree_guid, tree_name, leveling_type_guid, node_guid, node_name, level_requirement, skill_guid, skill_name, rank_guid, effect_guid)
		VALUES (900, 'FighterTree', 700, 901, 'GrantLv2', 2, 902, 'CleaveSkill', 850, 860)`)
	exec(t, raw, `INSERT INTO skill_rank_stat_mod (rank_guid, ord, stat_guid, stat_name, value)
		VALUES (850, 0, 10, 'Strength', 5.0)`)
	exec(t, raw, `INSERT INTO skill_point_type (guid, name) VALUES (810, 'Combat_Archetype')`)
	exec(t, raw, `INSERT INTO leveling_point (type_guid, level, point_type_guid, quantity)
		VALUES (700, 2, 810, 1)`)
	exec(t, raw, `INSERT INTO skill_tree (guid, name, kind, leveling_type_guid, class_guid, weapon_line_guid)
		VALUES (900, 'FighterTree', 'class', 700, 7, 0)`)
	exec(t, raw, `INSERT INTO skill_node (tree_guid, node_guid, ord, name, skill_guid, skill_name, level_requirement, point_requirement, auto_granted, auto_hotbar, all_required, opt, slot_guid)
		VALUES (900, 901, 0, 'CleaveNode', 902, 'CleaveSkill', 1, 0, 0, 1, 1, 0, 0)`)
	exec(t, raw, `INSERT INTO skill_node_require (tree_guid, node_guid, ord, skill_guid, slot_node_guid)
		VALUES (900, 901, 0, 903, 0), (900, 901, 1, 0, 904)`)
	exec(t, raw, `INSERT INTO skill_node_exclusive (tree_guid, node_guid, ord, skill_guid)
		VALUES (900, 901, 0, 905)`)
	exec(t, raw, `INSERT INTO skill_rank (skill_guid, ord, rank_guid, name, ability_guid, ability_name, effect_guid, effect_name, effect_infinite, effect_duration)
		VALUES (902, 0, 850, 'CleaveRank1', 860, 'Cleave', 870, 'CleaveBuff', 1, 0.0)`)
	exec(t, raw, `INSERT INTO skill_rank_cost (rank_guid, ord, point_type_guid, quantity)
		VALUES (850, 0, 810, 1)`)
}

func TestProgressionAndTalentReaders(t *testing.T) {
	db := openSeededDB(t, seedProgressionAndTalents)
	verifyProgressionQueries(t, db)
	verifyTalentQueries(t, db)
}

func verifyProgressionQueries(t *testing.T, db *DB) {
	t.Helper()
	iu, err := db.ItemUse(500)
	if err != nil || !iu.ConsumeOnUse || len(iu.Actions) != 1 || iu.Actions[0] != 600 {
		t.Fatalf("ItemUse(500): %+v (%v)", iu, err)
	}
	if _, err := db.ItemUse(999999); !IsNoRow(err) {
		t.Fatalf("expected IsNoRow for missing ItemUse, got %v", err)
	}
	ar, err := db.ActionResponse(600)
	if err != nil || len(ar.RewardTables) != 1 || ar.RewardTables[0] != 610 {
		t.Fatalf("ActionResponse(600): %+v (%v)", ar, err)
	}
	if _, err := db.ActionResponse(999999); !IsNoRow(err) {
		t.Fatalf("expected IsNoRow for missing ActionResponse, got %v", err)
	}
	rt, err := db.RewardTable(610)
	if err != nil || len(rt.Items) != 1 || len(rt.Experience) != 1 || rt.Experience[0].TargetLevel != 5 {
		t.Fatalf("RewardTable(610): %+v (%v)", rt, err)
	}
	if _, err := db.RewardTable(999999); !IsNoRow(err) {
		t.Fatalf("expected IsNoRow for missing RewardTable, got %v", err)
	}
	lt, err := db.LevelingTypeByName("Adventuring")
	if err != nil || lt.LevelCap != 50 || len(lt.Curve) != 2 {
		t.Fatalf("LevelingTypeByName: %+v (%v)", lt, err)
	}
	if _, err := db.LevelingTypeByName("MissingType"); !IsNoRow(err) {
		t.Fatalf("expected IsNoRow for missing LevelingTypeByName, got %v", err)
	}
	grants, err := db.AutoGrants(700)
	if err != nil || len(grants) != 1 || len(grants[0].Mods) != 1 || grants[0].Mods[0].Value != 5.0 {
		t.Fatalf("AutoGrants(700): %+v (%v)", grants, err)
	}
}

func verifyTalentQueries(t *testing.T, db *DB) {
	t.Helper()
	tree, err := db.SkillTreeByName("FighterTree")
	if err != nil || tree.Guid != 900 || tree.Kind != TreeClass {
		t.Fatalf("SkillTreeByName: %+v (%v)", tree, err)
	}
	if _, err := db.SkillTreeByName("MissingTree"); !IsNoRow(err) {
		t.Fatalf("expected IsNoRow for missing SkillTreeByName, got %v", err)
	}
	trees, err := db.SkillTrees()
	if err != nil || len(trees) != 1 {
		t.Fatalf("SkillTrees: %+v (%v)", trees, err)
	}
	nodes, err := db.SkillNodes(900)
	if err != nil || len(nodes) != 1 || len(nodes[0].Requires) != 1 || len(nodes[0].RequiresNodes) != 1 || len(nodes[0].Exclusive) != 1 {
		t.Fatalf("SkillNodes(900): %+v (%v)", nodes, err)
	}
	ranks, err := db.SkillRanks(902)
	if err != nil || len(ranks) != 1 || len(ranks[0].Costs) != 1 || !ranks[0].EffectInfinite {
		t.Fatalf("SkillRanks(902): %+v (%v)", ranks, err)
	}
	mods, err := db.RankStatMods(850)
	if err != nil || len(mods) != 1 || mods[0].StatName != "Strength" {
		t.Fatalf("RankStatMods(850): %+v (%v)", mods, err)
	}
	pts, err := db.SkillPointTypes()
	if err != nil || pts["Combat_Archetype"] != 810 {
		t.Fatalf("SkillPointTypes: %+v (%v)", pts, err)
	}
	lpts, err := db.LevelingPoints(700, 2)
	if err != nil || lpts[810] != 1 {
		t.Fatalf("LevelingPoints(700, 2): %+v (%v)", lpts, err)
	}
}

func seedNodesAndRuntimes(t *testing.T, raw *sql.DB) {
	t.Helper()
	exec(t, raw, `INSERT INTO city_node VALUES (0, 1000, 'Verra_RVR_Joeva', 'Joeva', 'Economic', 4, 'Verra',
		10.0, 20.0, 30.0, 90.0, 5000.0, 1, 2, 3, 1, 4, 5, 6, 7, 8, 1, 5, -100, 100, 3, 86400)`)
	exec(t, raw, `INSERT INTO city_zoi VALUES (3, 'ZOI_Joeva', 'HasNode', 0, 'Riverlands', 1, 1000, 'None', 1,
		-100.0, -200.0, -50.0, 100.0, 200.0, 50.0)`)
	exec(t, raw, `INSERT INTO city_zoi_adjacent VALUES (3, 0, 4)`)
	exec(t, raw, `INSERT INTO city_node_level VALUES (1000, 3, 2000)`)
	exec(t, raw, `INSERT INTO node_level_details VALUES (2000, 'Joeva_Lv3')`)
	exec(t, raw, `INSERT INTO node_level_details_mod VALUES (2000, 0, 3000)`)
	exec(t, raw, `INSERT INTO node_modification VALUES (3000, 'Mod_Relic', 2, 3, 4, 10, 1500.0, 0.1, 1)`)
	exec(t, raw, `INSERT INTO child_node_rule VALUES (4000, 'StandardRules', 4, 3, 2)`)
	exec(t, raw, `INSERT INTO vassalship_def VALUES (5000, 'StandardVassal', 4000, 5, 10, 3, 0.15, 3600)`)
	exec(t, raw, `INSERT INTO vassalship_region_size VALUES (5000, 2, 4)`)
	exec(t, raw, `INSERT INTO effect VALUES (30, 'Eff', 'Effect', 'Buff', '["t1"]', '[]', '', 1, '', 1, 1, 'Override', 50, 'Refresh', 0, 0, 0, 1)`)
	exec(t, raw, `INSERT INTO ability_runtime VALUES (20, 'Sprint', 'Smart', 0, 5.0, 10, 1.25, 1, 1.0, 0.0, 0, 0, 1, 30, '/Game/M', 'Cancel', 'auto')`)
	exec(t, raw, `INSERT INTO effect_runtime VALUES (30, 'Eff', 1, 0.0, 1.0, 0, 1, 31, 2.0, 'auto')`)
	exec(t, raw, `INSERT INTO effect_stat_runtime VALUES (30, 0, 10, 'Speed', 0, '', 0.0, 1.5)`)
}

func TestNodeAndRuntimeReaders(t *testing.T) {
	db := openSeededDB(t, seedNodesAndRuntimes)

	nodes, err := db.CityNodes()
	if err != nil || len(nodes) != 1 || nodes[0].NodeName != "Joeva" || !nodes[0].Levelable {
		t.Fatalf("CityNodes: %+v (%v)", nodes, err)
	}
	zois, err := db.CityZois()
	if err != nil || len(zois) != 1 || len(zois[0].Adjacent) != 1 || zois[0].Adjacent[0] != 4 {
		t.Fatalf("CityZois: %+v (%v)", zois, err)
	}
	lvlMap, err := db.CityNodeLevelDetails()
	if err != nil || lvlMap[1000][3] != 2000 {
		t.Fatalf("CityNodeLevelDetails: %+v (%v)", lvlMap, err)
	}
	details, err := db.NodeLevelDetailsList()
	if err != nil || len(details) != 1 || len(details[0].Modifications) != 1 || details[0].Modifications[0] != 3000 {
		t.Fatalf("NodeLevelDetailsList: %+v (%v)", details, err)
	}
	mods, err := db.NodeModifications()
	if err != nil || len(mods) != 1 || !mods[0].UnlocksRecipes || mods[0].RelicSlots != 2 {
		t.Fatalf("NodeModifications: %+v (%v)", mods, err)
	}
	rules, err := db.ChildNodeRules()
	if err != nil || len(rules) != 1 || rules[0].MaxChildren != 2 {
		t.Fatalf("ChildNodeRules: %+v (%v)", rules, err)
	}
	vdefs, err := db.VassalshipDefs()
	if err != nil || len(vdefs) != 1 || len(vdefs[0].RequiredRegionSizeAt) != 3 || vdefs[0].RequiredRegionSizeAt[2] != 4 {
		t.Fatalf("VassalshipDefs: %+v (%v)", vdefs, err)
	}
	if eff, err := db.Effect(30); err != nil || eff.Name != "Eff" {
		t.Fatalf("Effect(30): %+v (%v)", eff, err)
	}
	if ar, err := db.AbilityRuntimes(); err != nil || ar[20].Name != "Sprint" {
		t.Fatalf("AbilityRuntimes: %+v (%v)", ar, err)
	}
	if er, err := db.EffectRuntimes(); err != nil || er[30].EndEffectGuid != 31 {
		t.Fatalf("EffectRuntimes: %+v (%v)", er, err)
	}
	if esr, err := db.EffectStatRuntimes(); err != nil || len(esr[30]) != 1 || esr[30][0].Addend != 1.5 {
		t.Fatalf("EffectStatRuntimes: %+v (%v)", esr, err)
	}
}

func TestPlayerAppearanceReader(t *testing.T) {
	db := openSeededDB(t, func(t *testing.T, raw *sql.DB) {
		exec(t, raw, `INSERT INTO record VALUES
			(1, ?, 'RaceGenderAppearanceMapRecordBase', ?, 0, 'PlayerCharacter', 0,
			 '{"MaleTulnarId":{"Guid":0,"TypeId":100},"MaleKaelarId":{"Guid":200,"TypeId":100},"FemaleKaelarId":{"Guid":299,"TypeId":100}}'),
			(2, 100, 'RaceGenderAppearance', 200, 0, 'Kae_M_App', 0,
			 '{"CustomizationDefId":{"Guid":300,"TypeId":101},"SectionsValuesLimits":{"MinSectionsValues":[{"Key":"FCharacterCustomizationBodySections::Chest","Value":{"X":-1,"Y":-2,"Z":-3}}],"MaxSectionsValues":[{"Key":"Chest","Value":{"X":1,"Y":2,"Z":3}}]}}'),
			(3, 101, 'RaceGenderCustomizationDef', 300, 0, 'Kae_M', 0,
			 '{"HeadHairOptionsIds":[{"Guid":11,"TypeId":1}],"FacialLipHairOptionsIds":[],"FacialChinHairOptionsIds":[],"FacialCheekHairOptionsIds":[],"FacialBrowHairOptionsIds":[],"RacialHornOptionsIds":[],"PresetsIds":[{"Guid":22,"TypeId":2}],"SkinSetIds":[{"Guid":33,"TypeId":3},{"Guid":34,"TypeId":3}],"CustomizationEntitlementId":{"Guid":400,"TypeId":102}}'),
			(4, 102, 'CharacterCustomizationEntitlements', 400, 0, 'Kae_M_Ent', 0,
			 '{"IrisIDsIds":[{"Guid":44,"TypeId":4}],"ScleraIDsIds":[],"IrisColorIDsIds":[],"HairColorIDsIds":[],"NailColorIDsIds":[],"MorphsId":{"Guid":500,"TypeId":103}}'),
			(5, 103, 'CharacterMorph', 500, 0, 'Kae_M_Morph', 0,
			 '{"Morph":[{"Morphs":[{"MorphName":"NoseWidth","Min":-0.5,"Max":0.75}]}]}')`,
			int64(TypeRaceGenderAppearanceMap), int64(GuidPlayerAppearanceMap))
	})

	tulnar, err := db.PlayerAppearance("Tulnar", "Male")
	if err != nil || tulnar != nil {
		t.Fatalf("PlayerAppearance(Tulnar, Male) = %+v, %v; want nil, nil", tulnar, err)
	}
	kae, err := db.PlayerAppearance("Kaelar", "Male")
	if err != nil || kae == nil {
		t.Fatalf("PlayerAppearance(Kaelar, Male) = %+v, %v", kae, err)
	}
	if kae.Name != "Kae_M" || !kae.HeadHair.Has(11) || kae.HeadHair.Has(99) || kae.SkinSets != 2 || !kae.Iris.Has(44) {
		t.Fatalf("unexpected appearance sets: %+v", kae)
	}
	if kae.Morphs["nosewidth"] != (Range{Min: -0.5, Max: 0.75}) {
		t.Fatalf("unexpected morphs: %+v", kae.Morphs)
	}
	if kae.SectionMin["chest"] != (Vec3{-1, -2, -3}) || kae.SectionMax["chest"] != (Vec3{1, 2, 3}) {
		t.Fatalf("unexpected section limits: min=%+v max=%+v", kae.SectionMin, kae.SectionMax)
	}
	if _, err := db.PlayerAppearance("Kaelar", "Female"); err == nil {
		t.Fatal("expected error when referenced appearance record is missing")
	}
	if _, err := db.PlayerAppearance("UnknownRace", "Male"); err == nil {
		t.Fatal("expected error for unknown race field in appearance map")
	}
	emptyDB := openSeededDB(t, nil)
	if _, err := emptyDB.PlayerAppearance("Kaelar", "Male"); err == nil {
		t.Fatal("expected error when appearance map record is missing")
	}
}

func TestClosedDBAndCorruptRowErrors(t *testing.T) {
	closed := openSeededDB(t, nil)
	_ = closed.Close()

	assertClosedReaderErrors(t, closed)
	assertCorruptRowErrors(t)
}

func assertClosedReaderErrors(t *testing.T, closed *DB) {
	t.Helper()
	errs := []error{
		firstErr(closed.Combat()),
		firstErr(closed.FoliageDefs()),
		firstErr(closed.FoliageStates()),
		firstErr(closed.GatherInteractions()),
		firstErr(closed.GatherInteractionDefs()),
		firstErr(closed.GatherDefs()),
		firstErr(closed.Professions()),
		firstErr(closed.CertificationLevels()),
		firstErr(closed.EquipSlotNames()),
		firstErr(closed.ItemsNamedLike("%")),
		firstErr(closed.StartingKit("Bard")),
		firstErr(closed.ResourceBags()),
		firstErr(closed.SkillTrees()),
		firstErr(closed.SkillNodes(1)),
		firstErr(closed.SkillRanks(1)),
		firstErr(closed.RankStatMods(1)),
		firstErr(closed.SkillPointTypes()),
		firstErr(closed.LevelingPoints(1, 1)),
		firstErr(closed.AutoGrants(1)),
		firstErr(closed.CityNodes()),
		firstErr(closed.CityZois()),
		firstErr(closed.CityNodeLevelDetails()),
		firstErr(closed.NodeLevelDetailsList()),
		firstErr(closed.NodeModifications()),
		firstErr(closed.ChildNodeRules()),
		firstErr(closed.VassalshipDefs()),
		firstErr(closed.AbilityRuntimes()),
		firstErr(closed.EffectRuntimes()),
		firstErr(closed.EffectStatRuntimes()),
		firstErr(closed.RecordTables()),
		firstErr(closed.StatDefByName("x")),
		firstErr(closed.StatInits("x")),
		firstErr(closed.Curves()),
		firstErr(closed.Classes()),
		firstErr(closed.ClassStatBase("Bard", 1)),
		firstErr(closed.RoundingAmbiguous()),
		firstErr(closed.StaminaRules()),
		firstErr(closed.StaminaRegenDisables()),
		firstErr(closed.ClassAbilityGuids("Bard")),
		firstErr(closed.AbilityCosts(1)),
		firstErr(closed.AbilityEffectCosts(1)),
		firstErr(closed.AbilityMontages(1)),
		firstErr(closed.Mods("effect", 1)),
		firstErr(closed.HitEffects(1)),
		firstErr(closed.AbilityHits(1)),
		firstErr(closed.EffectHits(1)),
	}
	for i, err := range errs {
		if err == nil {
			t.Fatalf("closed DB reader #%d unexpectedly succeeded", i)
		}
	}
}

func firstErr[T any](_ T, err error) error { return err }

func assertCorruptRowErrors(t *testing.T) {
	t.Helper()
	db := openSeededDB(t, func(t *testing.T, raw *sql.DB) {
		exec(t, raw, `INSERT INTO stat_curve VALUES (5, 'BadInterp', 0, 1, 10, 'UnknownInterp', 0, 0)`)
		exec(t, raw, `INSERT INTO ability VALUES (20, 'BadTags', 'BadTags', 'not-json', '', '0', 0, '', '', 0, 0, 0, '[]', '', 0, 0, 0, 0, 0)`)
		exec(t, raw, `INSERT INTO ability VALUES (21, 'BadCombo', 'BadCombo', '[]', '', '0', 0, '', '', 0, 0, 0, 'not-json', '', 0, 0, 0, 0, 0)`)
		exec(t, raw, `INSERT INTO effect VALUES (30, 'BadEffTags', 'Eff', 'Buff', 'not-json', '[]', '', 1, '', 1, 1, '', 1, '', 0, 0, 0, 0)`)
		exec(t, raw, `INSERT INTO effect VALUES (31, 'BadEffETags', 'Eff', 'Buff', '[]', 'not-json', '', 1, '', 1, 1, '', 1, '', 0, 0, 0, 0)`)
		exec(t, raw, `INSERT INTO ab_expr VALUES (1, '1', 'not-json', 0, 0, ''), (2, '1', '{"bad": "1"}', 0, 0, '')`)
		exec(t, raw, `INSERT INTO ab_ability VALUES (100, 'BadAb', 'BadAb', 'not-json', '', '', '', '', 0, 0, '', '', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '{}')`)
		exec(t, raw, `INSERT INTO ab_montage VALUES (100, 0, '/A', 'not-json', '', 0, '', '[]', 0, NULL)`)
		exec(t, raw, `INSERT INTO ab_montage VALUES (101, 0, '/A', '[]', '', 0, '', 'not-json', 0, NULL)`)
		exec(t, raw, `INSERT INTO ab_effect VALUES (200, 'BadAbEff', '', '', 'not-json', '[]', 0, 0, 0, 0, '', 0, 0, '', 0, 0, 0, 0, 0, 0, '{}', 0)`)
		exec(t, raw, `INSERT INTO ab_effect VALUES (201, 'BadAbEff2', '', '', '[]', 'not-json', 0, 0, 0, 0, '', 0, 0, '', 0, 0, 0, 0, 0, 0, '{}', 0)`)
	})
	if _, err := db.Curves(); err == nil {
		t.Fatal("expected error for invalid curve interp")
	}
	if _, err := db.Ability(20); err == nil {
		t.Fatal("expected error for invalid ability tags JSON")
	}
	if _, err := db.Ability(21); err == nil {
		t.Fatal("expected error for invalid ability combo JSON")
	}
	if _, err := db.Effect(30); err == nil {
		t.Fatal("expected error for invalid effect tags JSON")
	}
	if _, err := db.Effect(31); err == nil {
		t.Fatal("expected error for invalid effect_tags JSON")
	}
	if _, err := db.Expr(1); err == nil {
		t.Fatal("expected error for invalid ab_expr terms JSON")
	}
	if _, err := db.Expr(2); err == nil {
		t.Fatal("expected error for non-integer ab_expr term key")
	}
	if _, err := db.AbilityRow(100); err == nil {
		t.Fatal("expected error for invalid ab_ability tags JSON")
	}
	if _, err := db.AbilityMontages(100); err == nil {
		t.Fatal("expected error for invalid ab_montage paths JSON")
	}
	if _, err := db.AbilityMontages(101); err == nil {
		t.Fatal("expected error for invalid ab_montage hit_keys JSON")
	}
	if _, err := db.EffectRow(200); err == nil {
		t.Fatal("expected error for invalid ab_effect tags JSON")
	}
	if _, err := db.EffectRow(201); err == nil {
		t.Fatal("expected error for invalid ab_effect effect_tags JSON")
	}
}
