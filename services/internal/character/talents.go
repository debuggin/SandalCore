// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"fmt"
	"regexp"

	"github.com/jackc/pgx/v5"
)

// Plan D1, stage 2: a character's talents (the ranks it bought per skill tree: character_skill_ranks, migration 0006) and its action
// bar (character_action_bar). worldd saves both with SaveProgress, whole: what it hands over replaces what is stored.

// TreeRanks is the ranks a character bought in one skill tree, in the order it bought them. The ranks a level grants by itself are not
// stored.
type TreeRanks struct {
	TreeGuid  int64   // the design database's SkillTree Guid
	RankGuids []int64 // SkillRank Guids
}

// ActionbarSlot is one slot of the action bar (a row of character_action_bar): FActionbarSlotUseData with its bank and slot.
type ActionbarSlot struct {
	Bank         int32 // 0-19
	Slot         int32 // 0-15
	UseType      int32 // EActionbarUseType, 1 Ability to 9 Dismount
	Instigator   int32 // 0-3
	AbilityID    int64
	SkillRankID  int64
	PetCommandID int64
	ItemRecordID int64
	ItemGUID     string // 32 lowercase hex digits, or empty
	ItemRarity   int32  // 0-15
	ItemQuality  int32  // 0 or more
	ComboButton  int32  // 0-15
}

// The limits of what a character stores: the biggest retail tree is 49 ranks, a tree list's own bound is 4,096.
const (
	MaxTreeRanks     = 4096
	MaxSkillTrees    = 128
	MaxBankIndex     = 19
	MaxSlotIndex     = 15
	MaxActionSlots   = 256
	maxUseType       = 9
	maxInstigator    = 3
	maxRarityOrCombo = 15
)

var itemGuidPattern = regexp.MustCompile(`^[0-9a-f]{32}$`)

// ValidateSkillRanks says what is wrong with a character's ranks: a tree or rank that is no record, a tree given twice or a rank twice
// in a tree, a tree with more than MaxTreeRanks, or more than MaxSkillTrees trees.
func ValidateSkillRanks(trees []TreeRanks) error {
	if len(trees) > MaxSkillTrees {
		return fmt.Errorf("%w: %d skill trees (at most %d)", ErrInvalid, len(trees), MaxSkillTrees)
	}
	seen := map[int64]bool{}
	for _, t := range trees {
		if t.TreeGuid <= 0 {
			return fmt.Errorf("%w: skill tree %d is no record", ErrInvalid, t.TreeGuid)
		}
		if seen[t.TreeGuid] {
			return fmt.Errorf("%w: skill tree %d is given twice", ErrInvalid, t.TreeGuid)
		}
		seen[t.TreeGuid] = true
		if len(t.RankGuids) > MaxTreeRanks {
			return fmt.Errorf("%w: skill tree %d has %d ranks (at most %d)", ErrInvalid, t.TreeGuid, len(t.RankGuids), MaxTreeRanks)
		}
		ranks := map[int64]bool{}
		for _, r := range t.RankGuids {
			if r <= 0 {
				return fmt.Errorf("%w: rank %d of skill tree %d is no record", ErrInvalid, r, t.TreeGuid)
			}
			if ranks[r] {
				return fmt.Errorf("%w: rank %d is in skill tree %d twice", ErrInvalid, r, t.TreeGuid)
			}
			ranks[r] = true
		}
	}
	return nil
}

// [C O A x C-Y-B]

// Validate says what is wrong with the slot: the constraints of the table, checked where the message can say which.
func (s ActionbarSlot) Validate() error {
	switch {
	case s.Bank < 0 || s.Bank > MaxBankIndex:
		return fmt.Errorf("%w: action bar bank %d is not 0 to %d", ErrInvalid, s.Bank, MaxBankIndex)
	case s.Slot < 0 || s.Slot > MaxSlotIndex:
		return fmt.Errorf("%w: action bar slot %d is not 0 to %d", ErrInvalid, s.Slot, MaxSlotIndex)
	case s.UseType < 1 || s.UseType > maxUseType:
		return fmt.Errorf("%w: action bar use type %d is not 1 to %d (an empty slot is not stored)", ErrInvalid, s.UseType, maxUseType)
	case s.Instigator < 0 || s.Instigator > maxInstigator:
		return fmt.Errorf("%w: action bar instigator %d is not 0 to %d", ErrInvalid, s.Instigator, maxInstigator)
	case s.ItemRarity < 0 || s.ItemRarity > maxRarityOrCombo:
		return fmt.Errorf("%w: action bar item rarity %d is not 0 to %d", ErrInvalid, s.ItemRarity, maxRarityOrCombo)
	case s.ComboButton < 0 || s.ComboButton > maxRarityOrCombo:
		return fmt.Errorf("%w: action bar combo button %d is not 0 to %d", ErrInvalid, s.ComboButton, maxRarityOrCombo)
	case s.ItemQuality < 0:
		return fmt.Errorf("%w: action bar item quality %d is negative", ErrInvalid, s.ItemQuality)
	case s.ItemGUID != "" && !itemGuidPattern.MatchString(s.ItemGUID):
		return fmt.Errorf("%w: action bar item guid %q is not 32 lowercase hex digits", ErrInvalid, s.ItemGUID)
	}
	return nil
}

// ValidateActionBar says what is wrong with a bar: a slot Validate refuses, a slot held twice, or more than MaxActionSlots.
func ValidateActionBar(bar []ActionbarSlot) error {
	if len(bar) > MaxActionSlots {
		return fmt.Errorf("%w: %d action bar slots (at most %d)", ErrInvalid, len(bar), MaxActionSlots)
	}
	seen := map[[2]int32]bool{}
	for _, s := range bar {
		if err := s.Validate(); err != nil {
			return err
		}
		key := [2]int32{s.Bank, s.Slot}
		if seen[key] {
			return fmt.Errorf("%w: action bar bank %d slot %d is held twice", ErrInvalid, s.Bank, s.Slot)
		}
		seen[key] = true
	}
	return nil
}

// SkillRanks returns the ranks the character bought, by tree guid, each tree's in purchase order. A character with none has an empty
// list; an unknown character is ErrNotFound.
func (s *Store) SkillRanks(ctx context.Context, characterID string) ([]TreeRanks, error) {
	if !idPattern.MatchString(characterID) {
		return nil, ErrNotFound
	}
	rows, err := s.pool.Query(ctx, `SELECT tree_guid, rank_guid FROM character_skill_ranks WHERE character_id = $1 ORDER BY tree_guid, ord`, characterID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []TreeRanks
	for rows.Next() {
		var tree, rank int64
		if err := rows.Scan(&tree, &rank); err != nil {
			return nil, err
		}
		if len(out) == 0 || out[len(out)-1].TreeGuid != tree {
			out = append(out, TreeRanks{TreeGuid: tree})
		}
		out[len(out)-1].RankGuids = append(out[len(out)-1].RankGuids, rank)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if len(out) == 0 {
		if err := s.mustExist(ctx, characterID); err != nil {
			return nil, err
		}
	}
	return out, nil
}

// CoA and C.Y.B

// ActionBar returns the character's action bar by bank then slot. A character that never arranged it has none; an unknown character is
// ErrNotFound.
func (s *Store) ActionBar(ctx context.Context, characterID string) ([]ActionbarSlot, error) {
	if !idPattern.MatchString(characterID) {
		return nil, ErrNotFound
	}
	rows, err := s.pool.Query(ctx, `SELECT bank, slot, use_type, instigator, ability_id, skill_rank_id, pet_command_id, item_record_id, item_guid,
		item_rarity, item_quality, combo_button FROM character_action_bar WHERE character_id = $1 ORDER BY bank, slot`, characterID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []ActionbarSlot
	for rows.Next() {
		var a ActionbarSlot
		if err := rows.Scan(&a.Bank, &a.Slot, &a.UseType, &a.Instigator, &a.AbilityID, &a.SkillRankID, &a.PetCommandID, &a.ItemRecordID, &a.ItemGUID,
			&a.ItemRarity, &a.ItemQuality, &a.ComboButton); err != nil {
			return nil, err
		}
		out = append(out, a)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if len(out) == 0 {
		if err := s.mustExist(ctx, characterID); err != nil {
			return nil, err
		}
	}
	return out, nil
}

func (s *Store) mustExist(ctx context.Context, characterID string) error {
	var exists bool
	if err := s.pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1)`, characterID).Scan(&exists); err != nil {
		return err
	}
	if !exists {
		return ErrNotFound
	}
	return nil
}

// replaceSkillRanks writes the character's ranks in the transaction: what is stored goes, what is given comes, each tree's in order.
func replaceSkillRanks(ctx context.Context, tx pgx.Tx, characterID string, trees []TreeRanks) error {
	if _, err := tx.Exec(ctx, `DELETE FROM character_skill_ranks WHERE character_id = $1`, characterID); err != nil {
		return err
	}
	var treeGuids []int64
	var ords []int32
	var rankGuids []int64
	for _, t := range trees {
		for ord, rank := range t.RankGuids {
			treeGuids = append(treeGuids, t.TreeGuid)
			ords = append(ords, int32(ord))
			rankGuids = append(rankGuids, rank)
		}
	}
	if len(rankGuids) == 0 {
		return nil
	}
	_, err := tx.Exec(ctx, `INSERT INTO character_skill_ranks (character_id, tree_guid, ord, rank_guid)
		SELECT $1, * FROM unnest($2::bigint[], $3::int[], $4::bigint[])`,
		characterID, treeGuids, ords, rankGuids)
	return err
}

// replaceActionBar writes the character's action bar in the transaction.
func replaceActionBar(ctx context.Context, tx pgx.Tx, characterID string, bar []ActionbarSlot) error {
	if _, err := tx.Exec(ctx, `DELETE FROM character_action_bar WHERE character_id = $1`, characterID); err != nil {
		return err
	}
	if len(bar) == 0 {
		return nil
	}
	n := len(bar)
	banks := make([]int32, n)
	slots := make([]int32, n)
	useTypes := make([]int32, n)
	instigators := make([]int32, n)
	abilityIDs := make([]int64, n)
	skillRankIDs := make([]int64, n)
	petCommandIDs := make([]int64, n)
	itemRecordIDs := make([]int64, n)
	itemGUIDs := make([]string, n)
	itemRarities := make([]int32, n)
	itemQualities := make([]int32, n)
	comboButtons := make([]int32, n)
	for i, a := range bar {
		banks[i] = a.Bank
		slots[i] = a.Slot
		useTypes[i] = a.UseType
		instigators[i] = a.Instigator
		abilityIDs[i] = a.AbilityID
		skillRankIDs[i] = a.SkillRankID
		petCommandIDs[i] = a.PetCommandID
		itemRecordIDs[i] = a.ItemRecordID
		itemGUIDs[i] = a.ItemGUID
		itemRarities[i] = a.ItemRarity
		itemQualities[i] = a.ItemQuality
		comboButtons[i] = a.ComboButton
	}
	_, err := tx.Exec(ctx, `INSERT INTO character_action_bar (character_id, bank, slot, use_type, instigator, ability_id, skill_rank_id, pet_command_id,
		item_record_id, item_guid, item_rarity, item_quality, combo_button)
		SELECT $1, * FROM unnest($2::int[], $3::int[], $4::int[], $5::int[], $6::bigint[], $7::bigint[], $8::bigint[], $9::bigint[], $10::text[], $11::int[], $12::int[], $13::int[])`,
		characterID, banks, slots, useTypes, instigators, abilityIDs, skillRankIDs, petCommandIDs, itemRecordIDs, itemGUIDs, itemRarities, itemQualities, comboButtons)
	return err
}
