// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admingateway

import (
	"cmp"
	"context"
	"fmt"
	"net/http"
	"slices"
	"strconv"
	"strings"
	"time"

	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	admincatalog "aocrop/services/internal/admin-catalog"
	worldstream "aocrop/services/internal/world-stream"
)

type PlayerInfo struct {
	ID          uint64  `json:"id"`
	Character   string  `json:"character"`
	CharacterID string  `json:"character_id,omitempty"`
	Account     string  `json:"account,omitempty"`
	ClassID     uint32  `json:"class_id,omitempty"`
	Level       uint32  `json:"level"`
	X           float64 `json:"x"`
	Y           float64 `json:"y"`
	Z           float64 `json:"z"`
	YawDegrees  float64 `json:"yaw_degrees,omitempty"`
	Health      uint32  `json:"health"`
	MaxHealth   uint32  `json:"max_health"`
	Copper      int64   `json:"copper,omitempty"`
	Mounted     bool    `json:"mounted,omitempty"`
	Dead        bool    `json:"dead,omitempty"`
	Online      bool    `json:"online"`
	Zone        string  `json:"zone"`
}

const (
	StreamLayerUnspecified   uint32 = uint32(worldadminv1.StreamLayer_STREAM_LAYER_UNSPECIFIED)
	StreamLayerPlayers       uint32 = uint32(worldadminv1.StreamLayer_STREAM_LAYER_PLAYERS)        // 1
	StreamLayerNpcs          uint32 = uint32(worldadminv1.StreamLayer_STREAM_LAYER_NPCS)           // 2
	StreamLayerResourceNodes uint32 = uint32(worldadminv1.StreamLayer_STREAM_LAYER_RESOURCE_NODES) // 4
	StreamLayerMounts        uint32 = uint32(worldadminv1.StreamLayer_STREAM_LAYER_MOUNTS)         // 8
	StreamLayerLootPiles     uint32 = uint32(worldadminv1.StreamLayer_STREAM_LAYER_LOOT_PILES)     // 16
	StreamLayerAll           uint32 = uint32(worldadminv1.StreamLayer_STREAM_LAYER_ALL)            // 31
	LayerPois                uint32 = 32                                                           // 1 << 5
	LayerMonsters            uint32 = 64                                                           // 1 << 6
	LayerDefaultAll          uint32 = StreamLayerAll | LayerPois | LayerMonsters                   // 127
)

type layerSelection struct {
	LayerMask     uint32 `json:"layer_mask"`
	StreamLayers  uint32 `json:"stream_layers"`
	Players       bool   `json:"players"`
	Npcs          bool   `json:"npcs"`
	Monsters      bool   `json:"monsters"`
	ResourceNodes bool   `json:"resource_nodes"`
	Mounts        bool   `json:"mounts"`
	LootPiles     bool   `json:"loot_piles"`
	Pois          bool   `json:"pois"`
}

func toStreamLayers(mask uint32) uint32 {
	sl := mask & StreamLayerAll
	if (mask & LayerMonsters) != 0 {
		sl |= StreamLayerNpcs
	}
	return sl & StreamLayerAll
}

func selectionFromMask(mask uint32) layerSelection {
	return layerSelection{
		LayerMask:     mask,
		StreamLayers:  toStreamLayers(mask),
		Players:       (mask & StreamLayerPlayers) != 0,
		Npcs:          (mask & StreamLayerNpcs) != 0,
		Monsters:      (mask & LayerMonsters) != 0,
		ResourceNodes: (mask & StreamLayerResourceNodes) != 0,
		Mounts:        (mask & StreamLayerMounts) != 0,
		LootPiles:     (mask & StreamLayerLootPiles) != 0,
		Pois:          (mask & LayerPois) != 0,
	}
}

func parseLayerSelection(r *http.Request) layerSelection {
	q := r.URL.Query()
	if rawMask := strings.TrimSpace(q.Get("layer_mask")); rawMask != "" {
		if parsed, err := strconv.ParseUint(rawMask, 10, 32); err == nil {
			return selectionFromMask(uint32(parsed))
		}
	}
	if !q.Has("layers") {
		return selectionFromMask(LayerDefaultAll)
	}
	raw := strings.TrimSpace(q.Get("layers"))
	if raw == "" || strings.EqualFold(raw, "none") {
		return selectionFromMask(0)
	}
	if parsed, err := strconv.ParseUint(raw, 10, 32); err == nil {
		return selectionFromMask(uint32(parsed))
	}
	var mask uint32
	for _, part := range strings.Split(raw, ",") {
		switch strings.ToLower(strings.TrimSpace(part)) {
		case "all", "*":
			mask = LayerDefaultAll
		case "players", "player":
			mask |= StreamLayerPlayers
		case "npcs", "npc":
			mask |= StreamLayerNpcs
		case "monsters", "monster", "mobs", "mob":
			mask |= LayerMonsters
		case "resource_nodes", "resource_node", "resources", "gatherables":
			mask |= StreamLayerResourceNodes
		case "mounts", "mount":
			mask |= StreamLayerMounts
		case "loot_piles", "loot_pile", "loot", "corpses":
			mask |= StreamLayerLootPiles
		case "pois", "poi", "nodes", "places":
			mask |= LayerPois
		}
	}
	return selectionFromMask(mask)
}

func (s *Server) handleWorldState(w http.ResponseWriter, r *http.Request, _ Role) {
	if !checkAcceptHeader(w, r) {
		return
	}
	filter, err := parseViewportFilter(r)
	if err != nil {
		writeProblem(w, r, http.StatusBadRequest, "invalid-viewport", err.Error())
		return
	}
	sel := parseLayerSelection(r)
	if sel.StreamLayers != 0 {
		s.pullWorldStreamSnapshot(r.Context(), filter, sel.StreamLayers)
	}
	players := make([]PlayerInfo, 0)
	entities := make([]worldstream.EntitySnapshot, 0)
	pois := make([]admincatalog.PlaceEntry, 0)
	nodes := make([]PmTilesNodeMeta, 0)

	if sel.Players {
		players = s.collectPlayersInViewport(r.Context(), filter)
		for _, p := range players {
			if p.Online {
				entities = append(entities, worldstream.EntitySnapshot{
					ID: fmt.Sprintf("player:%d:%s", p.ID, p.Character), Kind: "player",
					Name: p.Character, Zone: firstNonEmpty(p.Zone, "verra"),
					X: float64(p.X), Y: float64(p.Y), Z: float64(p.Z),
					HealthPct: computeHealthPct(p.Health, p.MaxHealth), Level: int(p.Level),
					State: "online", UpdatedAt: s.now().UnixMilli(),
				})
			}
		}
	}
	npcs, monsters, resourceNodes, mounts, lootPiles := s.collectNonPlayerLayers(filter, sel)
	entities = append(entities, npcs...)
	entities = append(entities, monsters...)
	entities = append(entities, resourceNodes...)
	entities = append(entities, mounts...)
	entities = append(entities, lootPiles...)

	if sel.Pois {
		liveLevels := s.fetchCityNodeLevels(r.Context())
		worldPlaces := s.fetchWorldPlaces(r.Context())
		nodes = buildMapMetaNodes(s.getPmTilesArchive(), liveLevels)
		pois, _ = s.catalog.SearchPlacesFiltered(r.Context(), admincatalog.PlaceFilter{
			Limit: 200, ExtraPlaces: worldPlaces, NodeLevels: liveLevels,
		})
	}
	payload := map[string]any{
		"schema_version": "1.0",
		"layer_mask":     sel.LayerMask,
		"stream_layers":  sel.StreamLayers,
		"layers":         sel,
		"players":        players,
		"npcs":           npcs,
		"monsters":       monsters,
		"resource_nodes": resourceNodes,
		"mounts":         mounts,
		"loot_piles":     lootPiles,
		"pois":           pois,
		"places":         pois,
		"nodes":          nodes,
		"entities":       entities,
		"count":          len(entities),
		"serverTimeMs":   s.now().UnixMilli(),
	}
	writeCachedJSONWithModTime(w, r, "private, no-cache", s.lastMutatedTime(), payload)
}

func (s *Server) pullWorldStreamSnapshot(ctx context.Context, filter worldstream.ViewportFilter, streamLayers uint32) {
	cli := s.getWorldStreamClient()
	if cli == nil || streamLayers == 0 {
		return
	}
	area := &worldadminv1.AreaBox{MinXCm: -2500000, MinYCm: -2500000, MaxXCm: 2500000, MaxYCm: 2500000}
	if filter.HasBounds {
		area = &worldadminv1.AreaBox{
			MinXCm: int32(filter.MinX), MinYCm: int32(filter.MinY),
			MaxXCm: int32(filter.MaxX), MaxYCm: int32(filter.MaxY),
		}
	}
	sctx, cancel := context.WithTimeout(ctx, 1500*time.Millisecond)
	defer cancel()
	stream, err := cli.Subscribe(s.outgoingCtx(sctx), &worldadminv1.SubscribeRequest{
		Area:   area,
		Detail: worldadminv1.StreamDetail_STREAM_DETAIL_ENTITIES,
		Layers: streamLayers & StreamLayerAll,
	})
	if err != nil || stream == nil {
		return
	}
	frame, err := stream.Recv()
	if err != nil || frame == nil || frame.GetSnapshot() == nil {
		return
	}
	nowMs := s.now().UnixMilli()
	for _, ent := range frame.GetSnapshot().GetEntities() {
		s.coalescer.UpsertEntity(protoEntityToSnapshot(ent, nowMs))
	}
}

func protoEntityToSnapshot(ent *worldadminv1.Entity, nowMs int64) worldstream.EntitySnapshot {
	kind := "npc"
	switch ent.GetKind() {
	case worldadminv1.EntityKind_ENTITY_KIND_PLAYER:
		kind = "player"
	case worldadminv1.EntityKind_ENTITY_KIND_NPC:
		kind = "npc"
	case worldadminv1.EntityKind_ENTITY_KIND_RESOURCE_NODE:
		kind = "resource_node"
	case worldadminv1.EntityKind_ENTITY_KIND_MOUNT:
		kind = "mount"
	case worldadminv1.EntityKind_ENTITY_KIND_LOOT_PILE:
		kind = "loot_pile"
	}
	state := "active"
	if (ent.GetState() & uint32(worldadminv1.EntityState_ENTITY_STATE_DEAD)) != 0 {
		state = "dead"
	} else if (ent.GetState() & uint32(worldadminv1.EntityState_ENTITY_STATE_IN_COMBAT)) != 0 {
		state = "combat"
	}
	hp := float64(ent.GetHealthPct())
	if hp > 100 {
		hp = 100
	}
	name := ent.GetName()
	if name == "" {
		name = fmt.Sprintf("%s:%d", kind, ent.GetId())
	}
	return worldstream.EntitySnapshot{
		ID: fmt.Sprintf("%s:%d", kind, ent.GetId()), Kind: kind, Name: name,
		Zone: "verra", X: float64(ent.GetXCm()), Y: float64(ent.GetYCm()), Z: float64(ent.GetZCm()),
		HealthPct: hp, Level: int(ent.GetLevel()), State: state, UpdatedAt: nowMs,
	}
}

func (s *Server) handleWorldPlayers(w http.ResponseWriter, r *http.Request, _ Role) {
	if !checkAcceptHeader(w, r) {
		return
	}
	params, ok := parsePageParams(w, r, 50, 200)
	if !ok {
		return
	}
	players := s.collectPlayers(r.Context())
	onlineParam := strings.ToLower(strings.TrimSpace(r.URL.Query().Get("online")))
	qLower := strings.ToLower(params.Query)
	filtered := make([]PlayerInfo, 0, len(players))
	for _, p := range players {
		if onlineParam == "true" && !p.Online {
			continue
		}
		if onlineParam == "false" && p.Online {
			continue
		}
		if qLower != "" && !matchesPlayerQuery(p, qLower) {
			continue
		}
		filtered = append(filtered, p)
	}
	sortPlayers(filtered, params.Sort)
	total := len(filtered)
	start := min(max(params.Offset, 0), total)
	end := min(start+params.Limit, total)
	paged := filtered[start:end]
	env, xPag := buildPaginatedEnvelope(r, paged, "players", total, params.Limit, params.Offset, params.Fields)
	w.Header().Set("X-Pagination", xPag)
	writeCachedJSONWithModTime(w, r, "private, no-cache", s.lastMutatedTime(), env)
}

func matchesPlayerQuery(p PlayerInfo, qLower string) bool {
	return strings.Contains(strings.ToLower(p.Character), qLower) ||
		strings.Contains(strings.ToLower(p.CharacterID), qLower) ||
		strings.Contains(strings.ToLower(p.Account), qLower) ||
		strings.Contains(strings.ToLower(p.Zone), qLower) ||
		strings.Contains(strconv.FormatUint(p.ID, 10), qLower)
}

func sortPlayers(items []PlayerInfo, sortSpec string) {
	field, desc := "character", false
	if s := strings.TrimSpace(sortSpec); s != "" {
		first := strings.TrimSpace(strings.Split(s, ",")[0])
		if strings.HasPrefix(first, "-") {
			desc, field = true, strings.ToLower(strings.TrimSpace(first[1:]))
		} else {
			field = strings.ToLower(strings.TrimPrefix(first, "+"))
		}
	}
	slices.SortFunc(items, func(a, b PlayerInfo) int {
		var c int
		switch field {
		case "level":
			c = cmp.Compare(a.Level, b.Level)
		case "copper", "gold":
			c = cmp.Compare(a.Copper, b.Copper)
		case "health":
			c = cmp.Compare(a.Health, b.Health)
		default:
			c = cmp.Compare(strings.ToLower(a.Character), strings.ToLower(b.Character))
		}
		if desc {
			c = -c
		}
		if c != 0 {
			return c
		}
		return cmp.Compare(a.ID, b.ID)
	})
}

func (s *Server) collectPlayers(ctx context.Context) []PlayerInfo {
	seen := make(map[string]bool)
	var out []PlayerInfo
	if cli := s.getWorldAdminClient(); cli != nil {
		rctx, cancel := context.WithTimeout(ctx, 3*time.Second)
		resp, err := callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.ListPlayersResponse, error) {
			return cli.ListPlayers(c, &worldadminv1.ListPlayersRequest{})
		})
		cancel()
		if err == nil && resp != nil {
			for i, p := range resp.GetPlayers() {
				key := strings.ToLower(p.GetName())
				seen[key] = true
				var x, y, z float64
				if pos := p.GetPosition(); pos != nil {
					x, y, z = pos.GetX(), pos.GetY(), pos.GetZ()
				}
				hp := uint32(100)
				if p.GetDead() {
					hp = 0
				}
				out = append(out, PlayerInfo{
					ID: uint64(i + 1), Character: p.GetName(), CharacterID: p.GetCharacterId(),
					Level: 1, X: x, Y: y, Z: z, YawDegrees: p.GetYawDegrees(),
					Health: hp, MaxHealth: 100, Mounted: p.GetMounted(), Dead: p.GetDead(),
					Online: true, Zone: "verra",
				})
			}
		}
	}
	for _, snap := range s.coalescer.Snapshot(worldstream.ViewportFilter{}) {
		if snap.Kind == "player" && !seen[strings.ToLower(snap.Name)] {
			seen[strings.ToLower(snap.Name)] = true
			out = append(out, PlayerInfo{
				Character: snap.Name, Level: uint32(snap.Level),
				X: snap.X, Y: snap.Y, Z: snap.Z,
				Health: uint32(snap.HealthPct), MaxHealth: 100, Online: true, Zone: snap.Zone,
			})
		}
	}
	for _, off := range s.offlinePlayers {
		if !seen[strings.ToLower(off.Character)] {
			seen[strings.ToLower(off.Character)] = true
			out = append(out, off)
		}
	}
	return out
}

func (s *Server) collectPlayersInViewport(ctx context.Context, filter worldstream.ViewportFilter) []PlayerInfo {
	all := s.collectPlayers(ctx)
	if !filter.HasBounds {
		return all
	}
	out := make([]PlayerInfo, 0, len(all))
	for _, p := range all {
		x, y := float64(p.X), float64(p.Y)
		if x >= filter.MinX && x <= filter.MaxX && y >= filter.MinY && y <= filter.MaxY {
			out = append(out, p)
		}
	}
	return out
}

func (s *Server) collectNonPlayerLayers(
	filter worldstream.ViewportFilter, sel layerSelection,
) ([]worldstream.EntitySnapshot, []worldstream.EntitySnapshot, []worldstream.EntitySnapshot, []worldstream.EntitySnapshot, []worldstream.EntitySnapshot) {
	npcs := make([]worldstream.EntitySnapshot, 0)
	monsters := make([]worldstream.EntitySnapshot, 0)
	resourceNodes := make([]worldstream.EntitySnapshot, 0)
	mounts := make([]worldstream.EntitySnapshot, 0)
	lootPiles := make([]worldstream.EntitySnapshot, 0)
	if !sel.Npcs && !sel.Monsters && !sel.ResourceNodes && !sel.Mounts && !sel.LootPiles {
		return npcs, monsters, resourceNodes, mounts, lootPiles
	}
	for _, snap := range s.coalescer.Snapshot(filter) {
		k := strings.ToLower(strings.TrimSpace(snap.Kind))
		switch k {
		case "player":
			continue
		case "resource_node", "resource", "gatherable":
			if sel.ResourceNodes {
				snap.Kind = "resource_node"
				resourceNodes = append(resourceNodes, snap)
			}
		case "mount":
			if sel.Mounts {
				snap.Kind = "mount"
				mounts = append(mounts, snap)
			}
		case "loot_pile", "loot", "corpse":
			if sel.LootPiles {
				snap.Kind = "loot_pile"
				lootPiles = append(lootPiles, snap)
			}
		default:
			if isMonsterSnapshot(snap) {
				if sel.Monsters {
					snap.Kind = "monster"
					monsters = append(monsters, snap)
				}
			} else if sel.Npcs {
				snap.Kind = "npc"
				npcs = append(npcs, snap)
			}
		}
	}
	return npcs, monsters, resourceNodes, mounts, lootPiles
}

func isMonsterSnapshot(snap worldstream.EntitySnapshot) bool {
	k := strings.ToLower(strings.TrimSpace(snap.Kind))
	if k == "monster" || k == "mob" || k == "hostile" || k == "boss" {
		return true
	}
	st := strings.ToLower(strings.TrimSpace(snap.State))
	if st == "hostile" || st == "combat" || st == "aggressive" {
		return true
	}
	if st == "friendly" || st == "vendor" || st == "guard" || st == "quest" || st == "citizen" {
		return false
	}
	nameLower := strings.ToLower(snap.Name)
	for _, friendly := range []string{"vendor", "merchant", "innkeeper", "guard", "citizen", "villager", "guide", "quest", "trainer", "smith", "banker"} {
		if strings.Contains(nameLower, friendly) {
			return false
		}
	}
	for _, hostile := range []string{
		"goblin", "wolf", "skeleton", "zombie", "bandit", "spider", "bear", "elemental",
		"dragon", "boss", "raider", "cultist", "minotaur", "orc", "troll", "demon",
		"undead", "slime", "boar", "scorpion", "wyvern", "golem", "stalker", "brute",
	} {
		if strings.Contains(nameLower, hostile) {
			return true
		}
	}
	return false
}

func computeHealthPct(health, maxHealth uint32) float64 {
	if maxHealth == 0 {
		return 100
	}
	return float64(health) * 100.0 / float64(maxHealth)
}
