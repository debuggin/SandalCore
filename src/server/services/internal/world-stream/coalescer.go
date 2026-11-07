// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package worldstream

import (
	"context"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const DefaultTickInterval = time.Second / 30

func ServiceTickHz() float64 {
	if v, err := strconv.ParseFloat(strings.TrimSpace(os.Getenv("AOC_SERVICE_TICK_HZ")), 64); err == nil && v >= 1 && v <= 240 {
		return v
	}
	return 30.0
}

type EntitySnapshot struct {
	ID                string  `json:"id"`
	Kind              string  `json:"kind"`
	Name              string  `json:"name"`
	Zone              string  `json:"zone"`
	X                 float64 `json:"x"`
	Y                 float64 `json:"y"`
	Z                 float64 `json:"z"`
	Yaw               float64 `json:"yaw"`
	HealthPct         float64 `json:"healthPct"`
	Level             int     `json:"level"`
	State             string  `json:"state,omitempty"`
	ResourceType      string  `json:"resource_type,omitempty"`
	ResourceTypeCamel string  `json:"resourceType,omitempty"`
	Quality           string  `json:"quality,omitempty"`
	NpcSubkind        string  `json:"npc_subkind,omitempty"`
	NpcSubkindCamel   string  `json:"npcSubkind,omitempty"`
	UpdatedAt         int64   `json:"updatedAtMs"`
}

func normalizeEntitySnapshot(e EntitySnapshot) EntitySnapshot {
	if e.ResourceType == "" && e.ResourceTypeCamel != "" {
		e.ResourceType = e.ResourceTypeCamel
	}
	if e.ResourceTypeCamel == "" && e.ResourceType != "" {
		e.ResourceTypeCamel = e.ResourceType
	}
	if e.NpcSubkind == "" && e.NpcSubkindCamel != "" {
		e.NpcSubkind = e.NpcSubkindCamel
	}
	if e.NpcSubkindCamel == "" && e.NpcSubkind != "" {
		e.NpcSubkindCamel = e.NpcSubkind
	}
	return e
}

type ViewportFilter struct {
	Zone      string
	HasBounds bool
	MinX      float64
	MinY      float64
	MaxX      float64
	MaxY      float64
}

func (f ViewportFilter) Matches(e EntitySnapshot) bool {
	if f.Zone != "" && !strings.EqualFold(f.Zone, e.Zone) {
		return false
	}
	if !f.HasBounds {
		return true
	}
	return e.X >= f.MinX && e.X <= f.MaxX && e.Y >= f.MinY && e.Y <= f.MaxY
}

type WorldDelta struct {
	Sequence     uint64           `json:"sequence"`
	ServerTimeMs int64            `json:"serverTimeMs"`
	Zone         string           `json:"zone,omitempty"`
	Added        []EntitySnapshot `json:"added"`
	Updated      []EntitySnapshot `json:"updated"`
	Removed      []string         `json:"removed"`
	DroppedTicks uint64           `json:"droppedTicks,omitempty"`
}

type Subscription struct {
	ID             string
	Filter         ViewportFilter
	ch             chan WorldDelta
	known          map[string]struct{}
	pendingAdded   map[string]EntitySnapshot
	pendingUpdated map[string]EntitySnapshot
	pendingRemoved map[string]struct{}
	seq            uint64
	dropped        atomic.Uint64
}

func (s *Subscription) Updates() <-chan WorldDelta {
	return s.ch
}

func (s *Subscription) DroppedCount() uint64 {
	return s.dropped.Load()
}

type Coalescer struct {
	mu           sync.RWMutex
	tickInterval time.Duration
	now          func() time.Time
	entities     map[string]EntitySnapshot
	sortedAll    []EntitySnapshot
	sortedDirty  bool
	subs         map[string]*Subscription
}

func sameEntityPayload(a, b EntitySnapshot) bool {
	a.UpdatedAt = b.UpdatedAt
	return a == b
}

func NewCoalescer(tickInterval time.Duration) *Coalescer {
	if tickInterval <= 0 {
		if hz := ServiceTickHz(); hz > 0 && hz != 30.0 {
			tickInterval = time.Duration(float64(time.Second) / hz)
		} else {
			tickInterval = DefaultTickInterval
		}
	}
	return &Coalescer{
		tickInterval: tickInterval,
		now:          time.Now,
		entities:     make(map[string]EntitySnapshot),
		subs:         make(map[string]*Subscription),
	}
}

func (c *Coalescer) UpsertEntity(e EntitySnapshot) {
	if e.ID == "" {
		return
	}
	e = normalizeEntitySnapshot(e)
	c.mu.Lock()
	defer c.mu.Unlock()
	prev, exists := c.entities[e.ID]
	if e.UpdatedAt == 0 {
		if exists && sameEntityPayload(prev, e) {
			e.UpdatedAt = prev.UpdatedAt
		} else {
			e.UpdatedAt = c.now().UnixMilli()
		}
	}
	if exists && prev == e {
		return
	}
	c.entities[e.ID] = e
	c.sortedDirty = true
	if len(c.subs) > 0 {
		for _, sub := range c.subs {
			applyUpsertToSub(sub, e)
		}
	}
}

func (c *Coalescer) ReplaceBatch(upserts []EntitySnapshot, shouldPrune func(EntitySnapshot) bool) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	nowMs, hasSubs, mutated := c.now().UnixMilli(), len(c.subs) > 0, false
	for _, e := range upserts {
		if e.ID == "" {
			continue
		}
		e = normalizeEntitySnapshot(e)
		prev, exists := c.entities[e.ID]
		if e.UpdatedAt == 0 {
			if exists && sameEntityPayload(prev, e) {
				e.UpdatedAt = prev.UpdatedAt
			} else {
				e.UpdatedAt = nowMs
			}
		}
		if exists && prev == e {
			continue
		}
		c.entities[e.ID] = e
		c.sortedDirty, mutated = true, true
		if hasSubs {
			for _, sub := range c.subs {
				applyUpsertToSub(sub, e)
			}
		}
	}
	if shouldPrune != nil {
		for id, ent := range c.entities {
			if shouldPrune(ent) {
				delete(c.entities, id)
				c.sortedDirty, mutated = true, true
				if hasSubs {
					for _, sub := range c.subs {
						applyRemoveToSub(sub, id)
					}
				}
			}
		}
	}
	return mutated
}

func (c *Coalescer) PlayerSnapshots() []EntitySnapshot {
	c.mu.RLock()
	defer c.mu.RUnlock()
	out := make([]EntitySnapshot, 0, 8)
	for _, e := range c.entities {
		if e.Kind == "player" {
			out = append(out, e)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

func applyUpsertToSub(sub *Subscription, e EntitySnapshot) {
	if sub.Filter.Matches(e) {
		delete(sub.pendingRemoved, e.ID)
		if _, ok := sub.known[e.ID]; ok {
			if _, added := sub.pendingAdded[e.ID]; added {
				sub.pendingAdded[e.ID] = e
			} else {
				sub.pendingUpdated[e.ID] = e
			}
			return
		}
		sub.known[e.ID] = struct{}{}
		sub.pendingAdded[e.ID] = e
		return
	}
	applyRemoveToSub(sub, e.ID)
}

func (c *Coalescer) RemoveEntity(id string) {
	if id == "" {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if _, exists := c.entities[id]; !exists {
		return
	}
	delete(c.entities, id)
	c.sortedDirty = true
	for _, sub := range c.subs {
		applyRemoveToSub(sub, id)
	}
}

func applyRemoveToSub(sub *Subscription, id string) {
	if _, ok := sub.known[id]; !ok {
		return
	}
	delete(sub.known, id)
	if _, wasAdded := sub.pendingAdded[id]; wasAdded {
		delete(sub.pendingAdded, id)
		return
	}
	delete(sub.pendingUpdated, id)
	sub.pendingRemoved[id] = struct{}{}
}

func (c *Coalescer) Snapshot(filter ViewportFilter) []EntitySnapshot {
	c.mu.RLock()
	if c.sortedDirty {
		c.mu.RUnlock()
		c.mu.Lock()
		if c.sortedDirty {
			all := make([]EntitySnapshot, 0, len(c.entities))
			for _, e := range c.entities {
				all = append(all, e)
			}
			sort.Slice(all, func(i, j int) bool { return all[i].ID < all[j].ID })
			c.sortedAll, c.sortedDirty = all, false
		}
		c.mu.Unlock()
		c.mu.RLock()
	}
	defer c.mu.RUnlock()
	if !filter.HasBounds && filter.Zone == "" {
		return append(make([]EntitySnapshot, 0, len(c.sortedAll)), c.sortedAll...)
	}
	out := make([]EntitySnapshot, 0, len(c.sortedAll))
	for _, e := range c.sortedAll {
		if filter.Matches(e) {
			out = append(out, e)
		}
	}
	return out
}

func (c *Coalescer) Subscribe(id string, filter ViewportFilter) *Subscription {
	c.mu.Lock()
	defer c.mu.Unlock()
	sub := &Subscription{
		ID:             id,
		Filter:         filter,
		ch:             make(chan WorldDelta, 1),
		known:          make(map[string]struct{}),
		pendingAdded:   make(map[string]EntitySnapshot),
		pendingUpdated: make(map[string]EntitySnapshot),
		pendingRemoved: make(map[string]struct{}),
	}
	for _, e := range c.entities {
		if filter.Matches(e) {
			sub.known[e.ID] = struct{}{}
			sub.pendingAdded[e.ID] = e
		}
	}
	c.subs[id] = sub
	return sub
}

func (c *Coalescer) Unsubscribe(id string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if sub, ok := c.subs[id]; ok {
		delete(c.subs, id)
		close(sub.ch)
	}
}

func (c *Coalescer) EntityCount() int {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return len(c.entities)
}

func (c *Coalescer) SubscriberCount() int {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return len(c.subs)
}

func (c *Coalescer) Run(ctx context.Context) {
	ticker := time.NewTicker(c.tickInterval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			c.Flush()
		}
	}
}

// Flush coalesces dirty state across all subscribers into 10 Hz delta frames without blocking on slow readers.
func (c *Coalescer) Flush() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	nowMs := c.now().UnixMilli()
	flushed := 0
	for _, sub := range c.subs {
		if flushSubscription(sub, nowMs) {
			flushed++
		}
	}
	return flushed
}

func flushSubscription(sub *Subscription, nowMs int64) bool {
	if len(sub.pendingAdded) == 0 && len(sub.pendingUpdated) == 0 && len(sub.pendingRemoved) == 0 {
		return false
	}
	sub.seq++
	curr := buildDelta(sub, sub.seq, nowMs)
	select {
	case prev := <-sub.ch:
		curr = mergeDeltas(prev, curr)
		sub.dropped.Add(1)
	default:
	}
	sub.ch <- curr
	return true
}

func buildDelta(sub *Subscription, seq uint64, nowMs int64) WorldDelta {
	added := make([]EntitySnapshot, 0, len(sub.pendingAdded))
	for _, e := range sub.pendingAdded {
		added = append(added, e)
	}
	sort.Slice(added, func(i, j int) bool { return added[i].ID < added[j].ID })

	updated := make([]EntitySnapshot, 0, len(sub.pendingUpdated))
	for _, e := range sub.pendingUpdated {
		updated = append(updated, e)
	}
	sort.Slice(updated, func(i, j int) bool { return updated[i].ID < updated[j].ID })

	removed := make([]string, 0, len(sub.pendingRemoved))
	for id := range sub.pendingRemoved {
		removed = append(removed, id)
	}
	sort.Strings(removed)

	clear(sub.pendingAdded)
	clear(sub.pendingUpdated)
	clear(sub.pendingRemoved)

	return WorldDelta{
		Sequence:     seq,
		ServerTimeMs: nowMs,
		Zone:         sub.Filter.Zone,
		Added:        added,
		Updated:      updated,
		Removed:      removed,
	}
}

func mergeDeltas(older, newer WorldDelta) WorldDelta {
	addedMap := make(map[string]EntitySnapshot, len(older.Added)+len(newer.Added))
	updatedMap := make(map[string]EntitySnapshot, len(older.Updated)+len(newer.Updated))
	removedSet := make(map[string]struct{}, len(older.Removed)+len(newer.Removed))

	for _, e := range older.Added {
		addedMap[e.ID] = e
	}
	for _, e := range older.Updated {
		updatedMap[e.ID] = e
	}
	for _, id := range older.Removed {
		removedSet[id] = struct{}{}
	}
	applyNewerDelta(newer, addedMap, updatedMap, removedSet)

	newer.Added = sortedSnapshots(addedMap)
	newer.Updated = sortedSnapshots(updatedMap)
	newer.Removed = sortedKeys(removedSet)
	newer.DroppedTicks = older.DroppedTicks + 1
	return newer
}

func applyNewerDelta(
	newer WorldDelta,
	addedMap map[string]EntitySnapshot,
	updatedMap map[string]EntitySnapshot,
	removedSet map[string]struct{},
) {
	for _, e := range newer.Added {
		if _, wasRemoved := removedSet[e.ID]; wasRemoved {
			delete(removedSet, e.ID)
			updatedMap[e.ID] = e
		} else {
			addedMap[e.ID] = e
		}
	}
	for _, e := range newer.Updated {
		if _, wasAdded := addedMap[e.ID]; wasAdded {
			addedMap[e.ID] = e
		} else {
			updatedMap[e.ID] = e
		}
	}
	for _, id := range newer.Removed {
		if _, wasAdded := addedMap[id]; wasAdded {
			delete(addedMap, id)
		} else {
			delete(updatedMap, id)
			removedSet[id] = struct{}{}
		}
	}
}

func sortedSnapshots(m map[string]EntitySnapshot) []EntitySnapshot {
	out := make([]EntitySnapshot, 0, len(m))
	for _, e := range m {
		out = append(out, e)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

func sortedKeys(m map[string]struct{}) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}
