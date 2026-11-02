// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"encoding/json"
	"fmt"
	"strings"

	"aocrop/services/internal/designdb"
)

type questCounts struct {
	quests  int
	objs    int
	missing int
	gated   int
}

func writeSingleQuest(w *writer, r designdb.Row) ([]int64, bool, error) {
	f := newFields(fmt.Sprintf("NarrativeQuestV2 %s (%d)", r.Name, r.Guid), r.Fields)
	activation, _, err := readRequirement(r.Fields["ActivationRule"])
	if err != nil {
		return nil, false, fmt.Errorf("%s: activation rule: %w", f.where, err)
	}
	startV, _ := f.get("OnStartActions")
	start, err := readStartActions(startV)
	if err != nil {
		return nil, false, fmt.Errorf("%s: start actions: %w", f.where, err)
	}
	var qd questDetailJSON
	if qd.OnSuccess, err = readActionList(r.Fields["OnSuccessActions"]); err != nil {
		return nil, false, fmt.Errorf("%s: success actions: %w", f.where, err)
	}
	if qd.OnFail, err = readActionList(r.Fields["OnFailureActions"]); err != nil {
		return nil, false, fmt.Errorf("%s: failure actions: %w", f.where, err)
	}
	if qd.OnTick, err = readActionList(r.Fields["OnTickActions"]); err != nil {
		return nil, false, fmt.Errorf("%s: tick actions: %w", f.where, err)
	}
	qd.Timeout = floatOf(r.Fields["Timeout"])
	if boolOf(r.Fields["bSucceedOnTimeout"]) {
		qd.TimeoutOK = 1
	}
	qd.Giver = idGuid(r.Fields["QuestGiverMapRegistrationId"])
	detail, err := json.Marshal(qd)
	if err != nil {
		return nil, false, err
	}
	objIDs := f.ids("ObjectiveIds")
	rewards := f.ids("Rewards")
	flavor := enumValue(f.str("Flavor"))
	locality := enumValue(f.str("Locality"))
	level := f.int("Level")
	canAbandon, repeatable, disabled, defAct := f.boolean("bCanAbandon"), f.boolean("bRepeatable"), f.boolean("bDisabled"), f.boolean("bDefaultActivate")
	if f.err != nil {
		return nil, false, f.err
	}
	if err := w.exec(`INSERT INTO quest (guid, name, flavor, locality, level, can_abandon, repeatable, disabled, default_activate, activation, start_actions, rewards, detail)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		r.Guid, r.Name, flavor, locality, level, b2i(canAbandon), b2i(repeatable), b2i(disabled), b2i(defAct), activation, start, jsonIDs(rewards), string(detail)); err != nil {
		return nil, false, err
	}
	return objIDs, activation != "", nil
}

func writeQuestObjectives(w *writer, questGuid int64, objIDs []int64, objectives map[int64]designdb.Row, killSets map[int64]bool) (int, int, error) {
	var objs, missing int
	for i, og := range objIDs {
		orow, ok := objectives[og]
		if !ok {
			missing++ // a quest that names an objective the data does not hold keeps its place, as a gap the reader skips
			continue
		}
		of := newFields(fmt.Sprintf("NarrativeObjectiveV2 %s (%d)", orow.Name, og), orow.Fields)
		typ := enumValue(of.str("ObjectiveType"))
		count := of.int("CompletionCount")
		awake, hidden, completes, optional := of.boolean("bDefaultAwake"), of.boolean("bDefaultHidden"), of.boolean("bCompletesQuest"), of.boolean("bOptionalObjective")
		if of.err != nil {
			return 0, 0, of.err
		}
		od, err := objectiveDetail(w, orow)
		if err != nil {
			return 0, 0, fmt.Errorf("%s: %w", of.where, err)
		}
		if typ == "Kill" {
			for _, sg := range idsOf(structOf(orow.Fields["KillObjectiveData"])["PopulationSets"]) {
				killSets[sg] = true
			}
		}
		if l, ok := orow.Fields["QuestItemRewards"].([]designdb.Value); ok {
			for _, e := range l {
				for _, sg := range idsOf(structOf(e)["PopulationSetQueries"]) {
					killSets[sg] = true
				}
			}
		}
		if err := w.exec(`INSERT INTO quest_objective (quest_guid, idx, guid, name, obj_type, completion_count, default_awake, default_hidden, completes_quest, optional, detail)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			questGuid, i, og, orow.Name, typ, count, b2i(awake), b2i(hidden), b2i(completes), b2i(optional), od); err != nil {
			return 0, 0, err
		}
		objs++
	}
	return objs, missing, nil
}

func writeQuestAndObjectiveRows(w *writer, src *source) (questCounts, int, error) {
	objectives := map[int64]designdb.Row{}
	for _, r := range src.rows["NarrativeObjectiveV2"] {
		objectives[r.Guid] = r
	}
	var qc questCounts
	killSets := map[int64]bool{}
	for _, r := range src.rows["NarrativeQuestV2"] {
		objIDs, gated, err := writeSingleQuest(w, r)
		if err != nil {
			return qc, 0, err
		}
		qc.quests++
		if gated {
			qc.gated++
		}
		objs, missing, err := writeQuestObjectives(w, r.Guid, objIDs, objectives, killSets)
		if err != nil {
			return qc, 0, err
		}
		qc.objs += objs
		qc.missing += missing
	}
	// the creatures of the population sets the Kill objectives name (a kill counts for the sets its creature stands in)
	var killMembers int
	for sg := range killSets {
		if err := w.exec(`INSERT OR IGNORE INTO quest_kill_set (set_guid, instance_guid) SELECT set_guid, instance_guid FROM npc_set_member WHERE set_guid = ?`, sg); err != nil {
			return qc, 0, err
		}
	}
	if err := w.tx.QueryRow(`SELECT COUNT(*) FROM quest_kill_set`).Scan(&killMembers); err != nil {
		return qc, 0, fmt.Errorf("quest_kill_set: %w", err)
	}
	return qc, killMembers, nil
}

// writeQuestDialogueAndOfferLinks writes the quests an NPC offers, the nodes that offer or confirm a quest, the nodes that take items for a quest (version 37), and the quests a zone of influence starts.
func writeQuestDialogueAndOfferLinks(w *writer, src *source) (int, int, int, int, error) {
	var offers, accepts, turnIns, zoiQuests int
	offered := func(kind string, rows []designdb.Row) error {
		for _, r := range rows {
			l, _ := r.Fields["OfferedQuests"].([]designdb.Value)
			for i, e := range l {
				s, ok := e.(designdb.Struct)
				if !ok {
					continue
				}
				q := idGuid(s["Quest"])
				if q == 0 {
					continue
				}
				if err := w.exec(`INSERT INTO npc_offered_quest (source_kind, source_guid, idx, quest_guid, dynamic_node) VALUES (?, ?, ?, ?, ?)`,
					kind, r.Guid, i, q, idGuid(s["DynamicNodeId"])); err != nil {
					return err
				}
				offers++
			}
		}
		return nil
	}
	if err := offered("instance", src.rows["PopulationInstanceRecordBase"]); err != nil {
		return 0, 0, 0, 0, err
	}
	if err := offered("asset", src.rows["PopulationAssetRecordBase"]); err != nil {
		return 0, 0, 0, 0, err
	}
	for _, r := range src.rows["DialogueNode"] {
		switch enumValue(strOf(r.Fields["NodeType"])) {
		case "AcceptQuest", "Confirmation":
			if err := w.exec(`INSERT INTO dialogue_accept (node_guid, quest_guid, on_success, objectives) VALUES (?, ?, ?, ?)`,
				r.Guid, idGuid(r.Fields["QuestId"]), idGuid(r.Fields["OnSuccessNode"]), jsonIDs(idsOf(r.Fields["ObjectiveIds"]))); err != nil {
				return 0, 0, 0, 0, err
			}
			accepts++
		case "TurnIn":
			if err := w.exec(`INSERT INTO dialogue_turnin (node_guid, quest_guid, on_success, objectives) VALUES (?, ?, ?, ?)`,
				r.Guid, idGuid(r.Fields["QuestId"]), idGuid(r.Fields["OnSuccessNode"]), jsonIDs(idsOf(r.Fields["ObjectiveIds"]))); err != nil {
				return 0, 0, 0, 0, err
			}
			turnIns++
		}
	}
	for _, r := range src.rows["ZOIRecordBase"] {
		for i, q := range idsOf(r.Fields["LocationQuests"]) {
			if err := w.exec(`INSERT INTO zoi_quest (zoi_guid, idx, quest_guid) VALUES (?, ?, ?)`, r.Guid, i, q); err != nil {
				return 0, 0, 0, 0, err
			}
			zoiQuests++
		}
	}
	return offers, accepts, turnIns, zoiQuests, nil
}

func readAttunePlaces(w *writer) (map[string][]int64, error) {
	rows, err := w.tx.Query(`SELECT guid, name, descriptor FROM attune_point`)
	if err != nil {
		return nil, fmt.Errorf("shrine_icd: read attune_point: %w", err)
	}
	places := map[string][]int64{}
	for rows.Next() {
		var g int64
		var name, desc string
		if err := rows.Scan(&g, &name, &desc); err != nil {
			rows.Close()
			return nil, err
		}
		// "RVR_Samias_Hope" and "Samia's Hope" are one place: the key is the letters and digits, lower case
		places[alnum(name)] = append(places[alnum(name)], g)
		if i := strings.Index(name, "_"); i == 3 {
			places[alnum(name[i+1:])] = append(places[alnum(name[i+1:])], g)
		}
		places[alnum(desc)] = append(places[alnum(desc)], g)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, err
	}
	rows.Close()
	return places, nil
}

// writeShrineICDs links the Emberspring interactables of the places (ICD_<Place>_Attunable_Respawn_Shrine -> the attune point of that place, by name): what an Interact objective names.
func writeShrineICDs(w *writer) (int, error) {
	places, err := readAttunePlaces(w)
	if err != nil {
		return 0, err
	}
	// the interactable definitions are read from the record table (written above): the quest builder needs only their names
	icds, err := w.tx.Query(`SELECT guid, name FROM record WHERE schema = 'InteractableComponentDefRecordBase' AND name LIKE 'ICD%Attunable%' ORDER BY guid`)
	if err != nil {
		return 0, fmt.Errorf("shrine_icd: read the interactable definitions: %w", err)
	}
	type icdRow struct {
		guid int64
		name string
	}
	var found []icdRow
	for icds.Next() {
		var r icdRow
		if err := icds.Scan(&r.guid, &r.name); err != nil {
			icds.Close()
			return 0, err
		}
		found = append(found, r)
	}
	if err := icds.Err(); err != nil {
		icds.Close()
		return 0, err
	}
	icds.Close()
	var shrineICDs int
	for _, r := range found {
		core := strings.TrimPrefix(r.name, "ICD_")
		i := strings.Index(core, "_Attunable")
		if !strings.HasPrefix(r.name, "ICD_") || i <= 0 {
			continue
		}
		list := dedupeIDs(places[alnum(core[:i])])
		if len(list) != 1 {
			continue // a place the attune points do not name, or two of them: no link
		}
		if err := w.exec(`INSERT INTO shrine_icd (icd_guid, attune_guid) VALUES (?, ?)`, r.guid, list[0]); err != nil {
			return 0, err
		}
		shrineICDs++
	}
	return shrineICDs, nil
}

// writeQuestItemLinks writes the items bound to quests, and the crate type of each crate item.
func writeQuestItemLinks(w *writer, src *source) (int, int, error) {
	var itemLinks, crateItems int
	for _, r := range src.rows["ItemRecordBase"] {
		if crate := idGuid(r.Fields["CrateId"]); crate != 0 {
			if err := w.exec(`INSERT INTO item_crate (item_guid, crate_guid) VALUES (?, ?)`, r.Guid, crate); err != nil {
				return 0, 0, err
			}
			crateItems++
		}
		link := func(kind string, quests []int64) error {
			seen := map[int64]bool{}
			for _, q := range quests {
				if seen[q] {
					continue
				}
				seen[q] = true
				if err := w.exec(`INSERT INTO item_quest (item_guid, quest_guid, kind) VALUES (?, ?, ?)`, r.Guid, q, kind); err != nil {
					return err
				}
				itemLinks++
			}
			return nil
		}
		if d, ok := r.Fields["DeleteOnQuestsRemovedData"].(designdb.Struct); ok {
			kind := "delete_all"
			if enumValue(strOf(d["DeleteType"])) == "OnlyOne" {
				kind = "delete_one"
			}
			if err := link(kind, idsOf(d["Quests"])); err != nil {
				return 0, 0, err
			}
		}
		if err := link("abandon", idsOf(r.Fields["AbandonQuestsOnDelete"])); err != nil {
			return 0, 0, err
		}
	}
	return itemLinks, crateItems, nil
}

func buildQuests(w *writer, src *source, opt *Options, meta map[string]string) error {
	for _, schema := range questRecordTypes {
		if _, err := src.need(schema); err != nil {
			return err
		}
	}
	qc, killMembers, err := writeQuestAndObjectiveRows(w, src)
	if err != nil {
		return err
	}
	offers, accepts, turnIns, zoiQuests, err := writeQuestDialogueAndOfferLinks(w, src)
	if err != nil {
		return err
	}
	shrineICDs, err := writeShrineICDs(w)
	if err != nil {
		return err
	}
	itemLinks, crateItems, err := writeQuestItemLinks(w, src)
	if err != nil {
		return err
	}
	meta["quests.quests"] = fmt.Sprintf("%d (%d with an activation rule)", qc.quests, qc.gated)
	meta["quests.objectives"] = fmt.Sprintf("%d (%d named by a quest but missing)", qc.objs, qc.missing)
	meta["quests.offers"] = fmt.Sprint(offers)
	meta["quests.accept_nodes"] = fmt.Sprint(accepts)
	meta["quests.turnin_nodes"] = fmt.Sprint(turnIns)
	meta["quests.zoi_quests"] = fmt.Sprint(zoiQuests)
	meta["quests.item_links"] = fmt.Sprint(itemLinks)
	meta["quests.crate_items"] = fmt.Sprint(crateItems)
	meta["quests.shrine_icds"] = fmt.Sprint(shrineICDs)
	meta["quests.kill_set_members"] = fmt.Sprint(killMembers)
	opt.logf("quests: %d quests (%d with an activation rule), %d objectives (%d missing), %d offers by NPCs, %d accept nodes, %d turn-in nodes, %d zone quests, %d item links, %d crate items, %d creatures of the kill objectives' sets, %d shrine interactables",
		qc.quests, qc.gated, qc.objs, qc.missing, offers, accepts, turnIns, zoiQuests, itemLinks, crateItems, killMembers, shrineICDs)
	return buildObjects(w, src, meta)
}
