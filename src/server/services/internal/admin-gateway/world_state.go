// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admingateway

import (
	"cmp"
	"context"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"

	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	admincatalog "aocrop/services/internal/admin-catalog"
	worldstream "aocrop/services/internal/world-stream"
	"google.golang.org/grpc"
)

type CityNodeState = PmTilesNodeMeta

type nodeGovernanceState struct {
	MayorGuid      string
	MayorName      string
	ElectionStatus string
}

type PlayerInfo struct {
	ID          uint64  `json:"id"`
	GUID        string  `json:"guid"`
	Name        string  `json:"name"`
	Character   string  `json:"character"`
	CharacterID string  `json:"character_id,omitempty"`
	Account     string  `json:"account,omitempty"`
	ClassID     uint32  `json:"class_id,omitempty"`
	Level       uint32  `json:"level"`
	X           float64 `json:"x"`
	Y           float64 `json:"y"`
	Z           float64 `json:"z"`
	XCm         float64 `json:"x_cm"`
	YCm         float64 `json:"y_cm"`
	ZCm         float64 `json:"z_cm"`
	YawDegrees  float64 `json:"yaw_degrees,omitempty"`
	Health      uint32  `json:"health"`
	MaxHealth   uint32  `json:"max_health"`
	Copper      int64   `json:"copper,omitempty"`
	Mounted     bool    `json:"mounted,omitempty"`
	Dead        bool    `json:"dead,omitempty"`
	Online      bool    `json:"online"`
	Zone        string  `json:"zone"`
}

func normalizePlayerInfo(p PlayerInfo) PlayerInfo {
	p.Character, p.Name = firstNonEmpty(p.Character, p.Name), firstNonEmpty(p.Name, firstNonEmpty(p.Character, p.Name))
	if p.X == 0 && p.XCm != 0 {
		p.X = p.XCm
	}
	if p.Y == 0 && p.YCm != 0 {
		p.Y = p.YCm
	}
	if p.Z == 0 && p.ZCm != 0 {
		p.Z = p.ZCm
	}
	p.XCm, p.YCm, p.ZCm, p.Zone = p.X, p.Y, p.Z, firstNonEmpty(p.Zone, "verra")
	if p.Level == 0 {
		p.Level = 1
	}
	if p.GUID == "" {
		p.GUID = firstNonEmpty(p.CharacterID, fmt.Sprintf("player:%d:%s", p.ID, p.Character))
	}
	return p
}

const (
	StreamLayerUnspecified   uint32 = uint32(worldadminv1.StreamLayer_STREAM_LAYER_UNSPECIFIED)
	StreamLayerPlayers       uint32 = uint32(worldadminv1.StreamLayer_STREAM_LAYER_PLAYERS)
	StreamLayerNpcs          uint32 = uint32(worldadminv1.StreamLayer_STREAM_LAYER_NPCS)
	StreamLayerResourceNodes uint32 = uint32(worldadminv1.StreamLayer_STREAM_LAYER_RESOURCE_NODES)
	StreamLayerMounts        uint32 = uint32(worldadminv1.StreamLayer_STREAM_LAYER_MOUNTS)
	StreamLayerLootPiles     uint32 = uint32(worldadminv1.StreamLayer_STREAM_LAYER_LOOT_PILES)
	StreamLayerAll           uint32 = uint32(worldadminv1.StreamLayer_STREAM_LAYER_ALL)
	LayerPois                uint32 = 32
	LayerMonsters            uint32 = 64
	LayerDefaultAll          uint32 = StreamLayerAll | LayerPois | LayerMonsters
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
		LayerMask: mask, StreamLayers: toStreamLayers(mask), Players: (mask & StreamLayerPlayers) != 0,
		Npcs: (mask & StreamLayerNpcs) != 0, Monsters: (mask & LayerMonsters) != 0, ResourceNodes: (mask & StreamLayerResourceNodes) != 0,
		Mounts: (mask & StreamLayerMounts) != 0, LootPiles: (mask & StreamLayerLootPiles) != 0, Pois: (mask & LayerPois) != 0,
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
	sel, q := parseLayerSelection(r), r.URL.Query()
	if sel.StreamLayers != 0 {
		s.pullWorldStreamSnapshot(r.Context(), filter, sel.StreamLayers)
	}
	players, entities, pois, nodes, onlinePlayersCount := make([]PlayerInfo, 0), make([]worldstream.EntitySnapshot, 0), make([]admincatalog.PlaceEntry, 0), make([]PmTilesNodeMeta, 0), 0
	stateTimeMs := s.lastMutatedTime().UnixMilli()
	if sel.Players {
		players = s.collectPlayersInViewport(r.Context(), filter)
		for _, p := range players {
			if p.Online {
				onlinePlayersCount++
				entities = append(entities, worldstream.EntitySnapshot{
					ID: fmt.Sprintf("player:%d:%s", p.ID, p.Character), Kind: "player", Name: p.Character, Zone: firstNonEmpty(p.Zone, "verra"),
					X: float64(p.X), Y: float64(p.Y), Z: float64(p.Z), HealthPct: computeHealthPct(p.Health, p.MaxHealth), Level: int(p.Level), State: "online", UpdatedAt: stateTimeMs,
				})
			}
		}
	}
	npcs, monsters, resourceNodes, mounts, lootPiles := s.collectNonPlayerLayers(filter, sel)
	entities = slices.Concat(entities, npcs, monsters, resourceNodes, mounts, lootPiles)
	for _, e := range entities {
		if e.UpdatedAt > stateTimeMs {
			stateTimeMs = e.UpdatedAt
		}
	}
	compact, includePois := q.Get("compact") == "1", q.Get("include_pois") != "0"
	if sel.Pois && includePois {
		nodes, pois = s.resolveWorldNodesAndPois(r.Context())
	}
	todHours, anchorHours, anchorMs := s.TimeOfDayHoursAt(time.UnixMilli(stateTimeMs)), 12.0, int64(0)
	anchorHours, anchorMs = s.TimeOfDayAnchor()
	payload := map[string]any{
		"schema_version": "1.0", "layer_mask": sel.LayerMask, "stream_layers": sel.StreamLayers, "layers": sel,
		"layer_counts": map[string]int{"players": onlinePlayersCount, "npcs": len(npcs), "monsters": len(monsters), "resource_nodes": len(resourceNodes), "mounts": len(mounts), "loot_piles": len(lootPiles), "pois": len(pois), "city_nodes": len(nodes)},
		"players": players, "workers": s.snapshotWorldWorkerStatuses(), "data_modes": s.resolveDataModes(), "service_tick_hz": worldstream.ServiceTickHz(),
		"time_of_day_hours": todHours, "time_of_day_label": formatTimeOfDayLabel(todHours), "time_of_day_anchor_hours": anchorHours, "time_of_day_anchor_ms": anchorMs, "time_of_day_rate": 9.0, "entities": entities, "count": len(entities), "serverTimeMs": stateTimeMs,
	}
	if !compact {
		payload["npcs"], payload["monsters"], payload["resource_nodes"], payload["mounts"], payload["loot_piles"] = npcs, monsters, resourceNodes, mounts, lootPiles
		payload["pois"], payload["places"], payload["nodes"], payload["city_nodes"] = pois, pois, nodes, nodes
	} else if includePois {
		payload["pois"], payload["nodes"] = pois, nodes
	}
	cc := "private, no-cache"
	if filter.HasBounds {
		cc = "no-cache"
	}
	writeCachedJSONWithModTime(w, r, cc, s.lastMutatedTime(), payload)
}

type cachedWorldNodesAndPois struct {
	nodes []PmTilesNodeMeta
	pois  []admincatalog.PlaceEntry
}

func (s *Server) resolveWorldNodesAndPois(ctx context.Context) ([]PmTilesNodeMeta, []admincatalog.PlaceEntry) {
	now, key := s.now(), fmt.Sprintf("world:pois:%d", s.lastMutatedUnixNano.Load())
	if v, ok := s.metaCache.get(key, now); ok {
		if c, ok := v.(cachedWorldNodesAndPois); ok {
			return c.nodes, c.pois
		}
	}
	liveLevels := s.fetchCityNodeLevels(ctx)
	worldPlaces := s.fetchWorldPlaces(ctx)
	nodes := s.buildMapMetaNodes(s.getPmTilesArchive(), liveLevels)
	pois, _ := s.catalog.SearchPlacesFiltered(ctx, admincatalog.PlaceFilter{Limit: 10000, ExtraPlaces: worldPlaces, NodeLevels: liveLevels})
	s.metaCache.set(key, cachedWorldNodesAndPois{nodes: nodes, pois: pois}, 1500*time.Millisecond, now)
	return nodes, pois
}

func (s *Server) pullWorldStreamSnapshot(ctx context.Context, _ worldstream.ViewportFilter, streamLayers uint32) {
	effLayers := streamLayers & StreamLayerAll
	if effLayers == 0 {
		return
	}
	now, warm := s.now(), s.coalescer.EntityCount() > 0
	s.mu.Lock()
	lastAt, ok := s.lastStreamAt[effLayers]
	if warm && ok && now.Sub(lastAt) < 250*time.Millisecond {
		s.mu.Unlock()
		return
	}
	s.lastStreamAt[effLayers] = now
	s.mu.Unlock()
	area, nowMs := &worldadminv1.AreaBox{MinXCm: -2500000, MinYCm: -1500000, MaxXCm: 1000000, MaxYCm: 2000000}, now.UnixMilli()
	if warm && ok {
		if s.streamPullInFlight.CompareAndSwap(false, true) {
			go func() {
				defer s.streamPullInFlight.Store(false)
				bgCtx, cancel := context.WithTimeout(context.Background(), 900*time.Millisecond)
				defer cancel()
				s.pullAllWorkersSync(bgCtx, area, effLayers, nowMs)
			}()
		}
		return
	}
	s.pullAllWorkersSync(ctx, area, effLayers, nowMs)
}

func (s *Server) pullAllWorkersSync(ctx context.Context, area *worldadminv1.AreaBox, effLayers uint32, nowMs int64) {
	var wg sync.WaitGroup
	for _, w := range s.worldWorkers {
		if s.workerStreamClient(w) != nil {
			wg.Add(1)
			go func(worker *WorldWorkerEndpoint) {
				defer wg.Done()
				s.pullOneWorkerStream(ctx, worker, area, effLayers, nowMs)
			}(w)
		}
	}
	wg.Wait()
}

func (s *Server) pullOneWorkerStream(ctx context.Context, w *WorldWorkerEndpoint, area *worldadminv1.AreaBox, effLayers uint32, _ int64) {
	s.syncWorkerTimeOfDayIfNeeded(ctx, w)
	cli := s.workerStreamClient(w)
	if cli == nil {
		return
	}
	sctx, cancel := context.WithTimeout(ctx, 900*time.Millisecond)
	defer cancel()
	stream, err := cli.Subscribe(
		s.outgoingCtx(sctx),
		&worldadminv1.SubscribeRequest{Area: area, Detail: worldadminv1.StreamDetail_STREAM_DETAIL_ENTITIES, Layers: effLayers},
		grpc.MaxCallRecvMsgSize(64*1024*1024),
	)
	if err != nil || stream == nil {
		return
	}
	frame, err := stream.Recv()
	if err != nil || frame == nil || frame.GetSnapshot() == nil {
		return
	}
	scope, rawEnts := strings.ToLower(strings.TrimSpace(firstNonEmpty(w.ScopeKey, w.WorkerID))), frame.GetSnapshot().GetEntities()
	seenIDs, batch := make(map[string]struct{}, len(rawEnts)), make([]worldstream.EntitySnapshot, 0, len(rawEnts))
	for _, ent := range rawEnts {
		snap := protoEntityToSnapshot(ent, 0)
		if scope != "" && scope != "verra" {
			snap.ID, snap.Zone = scope+":"+snap.ID, scope
		}
		if snap.Kind == "resource_node" {
			snap.Name, snap.ResourceType, snap.Quality = ent.GetName(), "", ""
			enrichResourceNodeSnapshot(&snap, scope, ent.GetId())
		}
		if (ent.GetState()&uint32(worldadminv1.EntityState_ENTITY_STATE_DEAD)) != 0 || s.isEntityDespawned(snap.ID) {
			continue
		}
		seenIDs[snap.ID] = struct{}{}
		batch = append(batch, snap)
	}
	s.replaceWorkerStreamBatch(scope, area, effLayers, batch, seenIDs)
}

func protoEntityToSnapshot(ent *worldadminv1.Entity, nowMs int64) worldstream.EntitySnapshot {
	kind := "npc"
	stFlags := ent.GetState()
	switch ent.GetKind() {
	case worldadminv1.EntityKind_ENTITY_KIND_PLAYER:
		kind = "player"
	case worldadminv1.EntityKind_ENTITY_KIND_NPC:
		if (stFlags&4) != 0 || (stFlags&uint32(worldadminv1.EntityState_ENTITY_STATE_IN_COMBAT)) != 0 || isHostileNpcName(ent.GetName()) {
			kind = "monster"
		}
	case worldadminv1.EntityKind_ENTITY_KIND_RESOURCE_NODE:
		kind = "resource_node"
	case worldadminv1.EntityKind_ENTITY_KIND_MOUNT:
		kind = "mount"
	case worldadminv1.EntityKind_ENTITY_KIND_LOOT_PILE:
		kind = "loot_pile"
	}
	state := "active"
	if (stFlags & uint32(worldadminv1.EntityState_ENTITY_STATE_DEAD)) != 0 {
		state = "dead"
	} else if (stFlags & uint32(worldadminv1.EntityState_ENTITY_STATE_IN_COMBAT)) != 0 {
		state = "combat"
	} else if (stFlags&4) != 0 || kind == "monster" {
		state = "hostile"
	}
	hp, name := min(float64(ent.GetHealthPct()), 100.0), firstNonEmpty(ent.GetName(), fmt.Sprintf("%s:%d", kind, ent.GetId()))
	snap := worldstream.EntitySnapshot{
		ID: fmt.Sprintf("%s:%d", kind, ent.GetId()), Kind: kind, Name: name,
		Zone: "verra", X: float64(ent.GetXCm()), Y: float64(ent.GetYCm()), Z: float64(ent.GetZCm()),
		Yaw: float64(ent.GetYaw()) * 360.0 / 256.0, HealthPct: hp, Level: int(ent.GetLevel()), State: state, UpdatedAt: nowMs,
	}
	if kind == "resource_node" {
		enrichResourceNodeSnapshot(&snap, snap.Zone, ent.GetId())
	}
	return snap
}

func enrichResourceNodeSnapshot(snap *worldstream.EntitySnapshot, scope string, rawID uint64) {
	raw := strings.TrimSpace(snap.Name)
	if lb, rb := strings.IndexByte(raw, '['), strings.LastIndexByte(raw, ']'); lb > 0 && rb > lb {
		snap.Name = strings.TrimSpace(raw[:lb])
		if parts := strings.SplitN(raw[lb+1:rb], "|", 2); len(parts) == 2 {
			snap.ResourceType, snap.Quality = strings.TrimSpace(parts[0]), strings.TrimSpace(parts[1])
		}
	}
	catName, catType, catQual, catLv := resolveRegionalGatherableDef(scope, rawID)
	if snap.Name == "" || snap.Name == "GatherableActor" || strings.HasPrefix(strings.ToLower(snap.Name), "resource_node:") || strings.HasPrefix(strings.ToLower(snap.Name), "gatherable") {
		snap.Name = catName
	}
	snap.ResourceType, snap.Quality = firstNonEmpty(snap.ResourceType, catType), firstNonEmpty(snap.Quality, catQual)
	if snap.Level <= 0 {
		snap.Level = catLv
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
		strings.Contains(strings.ToLower(p.GUID), qLower) ||
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
	seen, nowMs, hasLive := make(map[string]bool), s.now().UnixMilli(), s.hasAnyWorldAdminClient()
	out := s.collectMultiWorkerOnlinePlayers(ctx, seen)
	for _, snap := range s.coalescer.PlayerSnapshots() {
		nameKey := strings.ToLower(snap.Name)
		if hasLive && !seen[nameKey] && (snap.UpdatedAt <= 0 || nowMs-snap.UpdatedAt > 4000) {
			s.coalescer.RemoveEntity(snap.ID)
			s.evictPlayerHandover(snap.Name)
			continue
		}
		if !seen[nameKey] {
			seen[nameKey] = true
			out = append(out, normalizePlayerInfo(PlayerInfo{
				ID: uint64(len(out) + 1), GUID: snap.ID, Character: snap.Name, Level: uint32(snap.Level),
				X: snap.X, Y: snap.Y, Z: snap.Z, Health: uint32(snap.HealthPct), MaxHealth: 100, Online: true, Zone: snap.Zone,
			}))
		}
	}
	for _, off := range s.offlinePlayers {
		nameKey := strings.ToLower(firstNonEmpty(off.Character, off.Name))
		if !seen[nameKey] {
			seen[nameKey] = true
			out = append(out, normalizePlayerInfo(off))
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
	npcs, monsters, resourceNodes, mounts, lootPiles := make([]worldstream.EntitySnapshot, 0), make([]worldstream.EntitySnapshot, 0), make([]worldstream.EntitySnapshot, 0), make([]worldstream.EntitySnapshot, 0), make([]worldstream.EntitySnapshot, 0)
	if !sel.Npcs && !sel.Monsters && !sel.ResourceNodes && !sel.Mounts && !sel.LootPiles {
		return npcs, monsters, resourceNodes, mounts, lootPiles
	}
	for _, snap := range s.coalescer.Snapshot(filter) {
		switch strings.ToLower(strings.TrimSpace(snap.Kind)) {
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
	k, st := strings.ToLower(strings.TrimSpace(snap.Kind)), strings.ToLower(strings.TrimSpace(snap.State))
	if k == "monster" || k == "mob" || k == "hostile" || k == "boss" || st == "hostile" || st == "combat" || st == "aggressive" {
		return true
	}
	if st == "friendly" || st == "vendor" || st == "guard" || st == "quest" || st == "citizen" {
		return false
	}
	return isHostileNpcName(snap.Name)
}

func isHostileNpcName(name string) bool {
	nameLower := strings.ToLower(name)
	for _, friendly := range []string{"vendor", "merchant", "innkeeper", "guard", "citizen", "villager", "guide", "quest", "trainer", "smith", "banker"} {
		if strings.Contains(nameLower, friendly) {
			return false
		}
	}
	for _, hostile := range []string{
		"goblin", "wolf", "skeleton", "zombie", "bandit", "spider", "bear", "elemental", "dragon", "boss", "raider", "cultist", "minotaur", "orc", "troll", "demon",
		"undead", "slime", "boar", "scorpion", "wyvern", "golem", "stalker", "brute", "raven", "mycel", "rabid", "grem", "gryphon", "raptor", "bullywog", "crox",
		"scarab", "beetle", "broodling", "skinwalker", "hoarder", "construct", "ooze", "imp", "drake", "crab", "turtle", "deer", "stag", "ram", "buck", "doe", "fox", "badger",
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

func normalizeNodeKey(raw string) string {
	s := strings.TrimPrefix(strings.ToLower(strings.TrimSpace(raw)), "verra_rvr_")
	var b strings.Builder
	for _, r := range s {
		if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') {
			b.WriteRune(r)
		}
	}
	return b.String()
}

func findCanonicalNodeSpec(nodeIdent string) (admincatalog.LevelableNodeSpec, bool) {
	norm := normalizeNodeKey(nodeIdent)
	trimmed := strings.TrimSpace(nodeIdent)
	if norm == "" && trimmed == "" {
		return admincatalog.LevelableNodeSpec{}, false
	}
	for _, sp := range admincatalog.DefaultLevelableNodes() {
		guidStr := strconv.FormatInt(sp.GUID, 10)
		if guidStr == trimmed || normalizeNodeKey(sp.Name) == norm || normalizeNodeKey(sp.DisplayName) == norm {
			return sp, true
		}
	}
	return admincatalog.LevelableNodeSpec{}, false
}

func (s *Server) applyNodeGovernance(meta *PmTilesNodeMeta) {
	if meta.ElectionStatus == "" {
		meta.ElectionStatus = "none"
	}
	s.mu.Lock()
	gov, ok := s.nodeGovernance[meta.GUID]
	if !ok {
		gov, ok = s.nodeGovernance[normalizeNodeKey(meta.Name)]
	}
	s.mu.Unlock()
	if !ok {
		return
	}
	meta.MayorGuid = gov.MayorGuid
	meta.MayorName = gov.MayorName
	if gov.ElectionStatus != "" {
		meta.ElectionStatus = gov.ElectionStatus
	}
}

func (s *Server) SetCityNodeMayor(nodeName, mayorGuid, mayorName string) (CityNodeState, bool) {
	sp, ok := findCanonicalNodeSpec(nodeName)
	if !ok {
		return CityNodeState{}, false
	}
	guidStr := strconv.FormatInt(sp.GUID, 10)
	mayorGuid = strings.TrimSpace(mayorGuid)
	mayorName = strings.TrimSpace(mayorName)
	gov := nodeGovernanceState{ElectionStatus: "none"}
	if mayorName != "" || mayorGuid != "" {
		if mayorName == "" {
			mayorName = mayorGuid
		}
		gov = nodeGovernanceState{MayorGuid: mayorGuid, MayorName: mayorName, ElectionStatus: "active_term"}
	}
	s.mu.Lock()
	s.nodeGovernance[guidStr] = gov
	s.nodeGovernance[normalizeNodeKey(sp.Name)] = gov
	lv, hasLv := s.localNodeLevels[guidStr]
	s.mu.Unlock()
	if !hasLv {
		lv = defaultNodeLevel(sp.Name)
	}
	s.recordMutation()
	return CityNodeState{
		NodeIndex: sp.NodeIndex, GUID: guidStr, Name: sp.Name, DisplayName: sp.DisplayName,
		TextureStem: sp.TextureStem, Ox: sp.Ox, Oy: sp.Oy, Oz: sp.Oz, YawDeg: sp.YawDeg,
		SizeCm: sp.SizeCm, Level: lv, PlugTier: nodePlugTierForLevel(lv),
		StageName: admincatalog.StageNameForLevel(lv), MayorGuid: gov.MayorGuid,
		MayorName: gov.MayorName, ElectionStatus: gov.ElectionStatus,
	}, true
}

func (s *Server) TriggerCityNodeElection(nodeName, action string) (CityNodeState, bool) {
	sp, ok := findCanonicalNodeSpec(nodeName)
	if !ok {
		return CityNodeState{}, false
	}
	guidStr := strconv.FormatInt(sp.GUID, 10)
	s.mu.Lock()
	gov := s.nodeGovernance[guidStr]
	switch strings.ToLower(strings.TrimSpace(action)) {
	case "start", "registration", "open_registration":
		gov.ElectionStatus = "registration"
	case "open_voting", "voting", "vote":
		gov.ElectionStatus = "voting"
	case "conclude", "finish", "complete", "active_term":
		gov.ElectionStatus = "active_term"
		if gov.MayorName == "" {
			gov.MayorName = "Elected Mayor"
		}
	case "cancel", "clear", "none", "reset":
		gov.ElectionStatus = "none"
	default:
		s.mu.Unlock()
		return CityNodeState{}, false
	}
	s.nodeGovernance[guidStr] = gov
	s.nodeGovernance[normalizeNodeKey(sp.Name)] = gov
	lv, hasLv := s.localNodeLevels[guidStr]
	s.mu.Unlock()
	if !hasLv {
		lv = defaultNodeLevel(sp.Name)
	}
	s.recordMutation()
	return CityNodeState{
		NodeIndex: sp.NodeIndex, GUID: guidStr, Name: sp.Name, DisplayName: sp.DisplayName,
		TextureStem: sp.TextureStem, Ox: sp.Ox, Oy: sp.Oy, Oz: sp.Oz, YawDeg: sp.YawDeg,
		SizeCm: sp.SizeCm, Level: lv, PlugTier: nodePlugTierForLevel(lv),
		StageName: admincatalog.StageNameForLevel(lv), MayorGuid: gov.MayorGuid,
		MayorName: gov.MayorName, ElectionStatus: gov.ElectionStatus,
	}, true
}

func (s *Server) RemoveEntity(entityGuid string) {
	id := strings.TrimSpace(entityGuid)
	if id == "" {
		return
	}
	parts := strings.Split(id, ":")
	rawID, _ := strconv.ParseUint(parts[len(parts)-1], 10, 64)
	s.markEntityDespawned(id, "", rawID)
	s.evictPlayerHandover(id)
	s.evictPlayerHandover(parts[len(parts)-1])
	for _, prefix := range []string{"", "npc:", "monster:", "player:"} {
		s.coalescer.RemoveEntity(prefix + strings.ToLower(id))
		s.coalescer.RemoveEntity(prefix + id)
	}
	for _, snap := range s.coalescer.Snapshot(worldstream.ViewportFilter{}) {
		if strings.EqualFold(snap.ID, id) || strings.EqualFold(snap.Name, id) || strings.HasSuffix(strings.ToLower(snap.ID), ":"+strings.ToLower(id)) || (rawID > 0 && strings.HasSuffix(snap.ID, fmt.Sprintf(":%d", rawID))) {
			s.coalescer.RemoveEntity(snap.ID)
			s.evictPlayerHandover(snap.Name)
		}
	}
}

func (s *Server) serveSSEStream(w http.ResponseWriter, r *http.Request, sub *worldstream.Subscription, maxFrames int) {
	rc := http.NewResponseController(w)
	_ = rc.SetReadDeadline(time.Time{})
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Connection", "keep-alive")
	w.WriteHeader(http.StatusOK)
	_ = rc.Flush()
	s.streamSubscriptionLoop(r, sub, maxFrames, func(payload []byte) error {
		_ = rc.SetWriteDeadline(s.now().Add(5 * time.Second))
		_, err := fmt.Fprintf(w, "event: delta\ndata: %s\n\n", payload)
		if err == nil {
			err = rc.Flush()
		}
		return err
	})
}

func (s *Server) serveWebSocketStream(w http.ResponseWriter, r *http.Request, sub *worldstream.Subscription, maxFrames int) {
	rc := http.NewResponseController(w)
	conn, rw, err := rc.Hijack()
	if err != nil {
		writeProblem(w, r, http.StatusInternalServerError, "hijack-failed", "WebSocket upgrade hijack failed.")
		return
	}
	defer conn.Close()
	sum := sha1.Sum([]byte(strings.TrimSpace(r.Header.Get("Sec-WebSocket-Key")) + wsMagicGUID))
	handshake := "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + base64.StdEncoding.EncodeToString(sum[:]) + "\r\n\r\n"
	if _, err := rw.WriteString(handshake); err != nil || rw.Flush() != nil {
		return
	}
	s.streamSubscriptionLoop(r, sub, maxFrames, func(payload []byte) error {
		_ = conn.SetWriteDeadline(s.now().Add(5 * time.Second))
		return writeWSFrame(conn, payload)
	})
}

func (s *Server) streamSubscriptionLoop(r *http.Request, sub *worldstream.Subscription, maxFrames int, emit func([]byte) error) {
	s.coalescer.Flush()
	for sent := 0; ; {
		select {
		case <-r.Context().Done():
			return
		case delta, ok := <-sub.Updates():
			if !ok {
				return
			}
			payload, _ := json.Marshal(delta)
			if emit(payload) != nil {
				return
			}
			if sent++; maxFrames > 0 && sent >= maxFrames {
				return
			}
		}
	}
}

func writeWSFrame(conn net.Conn, payload []byte) error {
	length, header := len(payload), []byte{0x81}
	if length <= 125 {
		header = append(header, byte(length))
	} else if length <= 65535 {
		header = append(header, 126, byte(length>>8), byte(length))
	} else {
		header = append(header, 127, 0, 0, 0, 0, 0, 0, 0, 0)
		binary.BigEndian.PutUint64(header[2:10], uint64(length))
	}
	if _, err := conn.Write(header); err != nil {
		return err
	}
	_, err := conn.Write(payload)
	return err
}
