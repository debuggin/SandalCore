// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admingateway

import (
	"encoding/json"
	"fmt"
	"net/http"
	"testing"
)

type botClassSpec struct {
	Archetype   string
	PrimaryItem string
	MountItem   string
}

var eightBotArchetypes = []botClassSpec{
	{Archetype: "Fighter", PrimaryItem: "606463310000000101", MountItem: "606463220000000001"},
	{Archetype: "Tank", PrimaryItem: "606463310000000201", MountItem: "606463220000000001"},
	{Archetype: "Rogue", PrimaryItem: "606463310000000301", MountItem: "606463220000000001"},
	{Archetype: "Ranger", PrimaryItem: "606463310000000401", MountItem: "606463220000000001"},
	{Archetype: "Mage", PrimaryItem: "606463310000000501", MountItem: "606463220000000002"},
	{Archetype: "Summoner", PrimaryItem: "606463310000000601", MountItem: "606463220000000002"},
	{Archetype: "Cleric", PrimaryItem: "606463310000000701", MountItem: "606463220000000003"},
	{Archetype: "Bard", PrimaryItem: "606463310000000801", MountItem: "606463220000000003"},
}

func TestSixteenMaxLevelPlayerBotsPatternsOfLifeAndCrossBorderHandover(t *testing.T) {
	waRvr := &fakeWorldAdminClient{}
	waSnd := &fakeWorldAdminClient{}
	wsRvr := &fakeWorldStreamClient{}
	wsSnd := &fakeWorldStreamClient{}
	srv := NewRouter(Config{
		WorldWorkers: []*WorldWorkerEndpoint{
			{WorkerID: "world-riverlands", ScopeKey: "riverlands", ScopeKind: "region", ContainerName: "aoc-world-riverlands", AdminClient: waRvr, StreamClient: wsRvr},
			{WorkerID: "world-sandsquall", ScopeKey: "sandsquall", ScopeKind: "region", ContainerName: "aoc-world-sandsquall", AdminClient: waSnd, StreamClient: wsSnd},
		},
		CityNodeClient: &fakeCityNodeClient{},
	})
	hdrs := map[string]string{
		"Content-Type": "application/json",
		"Accept":       "application/json",
	}

	botCount := 0
	for rep := 1; rep <= 4; rep++ {
		for _, cls := range eightBotArchetypes {
			botCount++
			botName := fmt.Sprintf("BotLv50_%s_%d", cls.Archetype, rep)
			verifySingleMaxLevelBotLifecycle(t, srv, hdrs, botName, cls, rep == 1)
		}
	}
	if botCount != 32 {
		t.Fatalf("expected 32 Max-Level (Lv 50) bots (4 per class across 8 classes), got %d", botCount)
	}
	pairCount := 0
	for _, actorCls := range eightBotArchetypes {
		for _, observerCls := range eightBotArchetypes {
			pairCount++
			actorName := fmt.Sprintf("ActorLv50_%s", actorCls.Archetype)
			observerName := fmt.Sprintf("ObserverLv50_%s", observerCls.Archetype)
			verifyActorObserverBidirectionalPair(t, srv, hdrs, actorName, observerName)
		}
	}
	if pairCount != 64 {
		t.Fatalf("expected all 8x8=64 (ActorArchetype, ObserverArchetype) combinations, got %d", pairCount)
	}
}

func verifyActorObserverBidirectionalPair(t *testing.T, srv *Server, hdrs map[string]string, actorName, observerName string) {
	t.Helper()
	for _, pair := range [][2]string{{actorName, observerName}, {observerName, actorName}} {
		actBody := []byte(fmt.Sprintf(`{"command":"step_player_border","character":%q,"x_cm":-454000,"y_cm":-568000}`, pair[0]))
		obsBody := []byte(fmt.Sprintf(`{"command":"step_player_border","character":%q,"x_cm":-453850,"y_cm":-568000}`, pair[1]))
		if rec := serveAuthed(srv, http.MethodPost, "/api/v1/gm/commands", "gm-architect-dev-token", hdrs, actBody); rec.Code != http.StatusOK {
			t.Fatalf("actor %s status = %d", pair[0], rec.Code)
		}
		if rec := serveAuthed(srv, http.MethodPost, "/api/v1/gm/commands", "gm-architect-dev-token", hdrs, obsBody); rec.Code != http.StatusOK {
			t.Fatalf("observer %s status = %d", pair[1], rec.Code)
		}
	}
}

func verifySingleMaxLevelBotLifecycle(t *testing.T, srv *Server, hdrs map[string]string, botName string, cls botClassSpec, spawnDummy bool) {
	t.Helper()
	commands := [][]byte{
		[]byte(fmt.Sprintf(`{"command":"give_item","character":%q,"item":%q,"count":1,"rarity":"Legendary"}`, botName, cls.PrimaryItem)),
		[]byte(fmt.Sprintf(`{"command":"give_item","character":%q,"item":%q,"count":1,"rarity":"Epic"}`, botName, cls.MountItem)),
		[]byte(fmt.Sprintf(`{"command":"give_item","character":%q,"item":"MonsterTrophy_GoblinFang","count":2,"rarity":"Rare"}`, botName)),
		[]byte(fmt.Sprintf(`{"command":"give_money","character":%q,"gold":50,"silver":25,"copper":75}`, botName)),
	}
	if spawnDummy {
		commands = append(commands, []byte(`{"command":"spawn_npc","npc":"Goblin","place":"Winstead","level":50}`))
	}
	commands = append(commands,
		[]byte(fmt.Sprintf(`{"command":"step_player_border","character":%q,"x_cm":-454000,"y_cm":-568000}`, botName)),
		[]byte(fmt.Sprintf(`{"command":"step_player_border","character":%q,"x_cm":-454000,"y_cm":2400,"border_distance_cm":2400}`, botName)),
		[]byte(fmt.Sprintf(`{"command":"step_player_border","character":%q,"x_cm":-619803,"y_cm":873514}`, botName)),
		[]byte(fmt.Sprintf(`{"command":"step_player_border","character":%q,"x_cm":-619803,"y_cm":873514}`, botName)),
		[]byte(fmt.Sprintf(`{"command":"step_player_border","character":%q,"x_cm":-619803,"y_cm":873514}`, botName)),
	)
	for i, body := range commands {
		rec := serveAuthed(srv, http.MethodPost, "/api/v1/gm/commands", "gm-architect-dev-token", hdrs, body)
		if rec.Code != http.StatusOK {
			t.Fatalf("bot %s step %d status = %d, want 200; body = %s", botName, i, rec.Code, rec.Body.String())
		}
		if i == len(commands)-1 {
			var doc map[string]any
			if err := json.Unmarshal(rec.Body.Bytes(), &doc); err != nil {
				t.Fatalf("decode final border handover for %s: %v", botName, err)
			}
			if doc["authoritative_region"] != "sandsquall" || doc["authoritative_worker"] != "aoc-world-sandsquall" {
				t.Fatalf("bot %s expected cross-border handover to aoc-world-sandsquall, got %#v", botName, doc)
			}
		}
	}
}
