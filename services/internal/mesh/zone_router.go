// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package mesh

import (
	"encoding/binary"
	"errors"
	"math"
	"slices"
	"sync"
)

const (
	MeshForwardHeaderSize       = 12
	DefaultBoundaryHysteresisCm = 20000.0
)

var (
	ErrTruncatedMeshHeader = errors.New("mesh forward header truncated: need 12 bytes")
	ErrStaleAuthorityEpoch = errors.New("stale authority epoch rejected by monotonic fence")
	ErrUnknownZone         = errors.New("zone not registered in router")
	ErrInvalidZoneBounds   = errors.New("invalid spatial zone bounds")
)

type HandoverState uint8

const (
	HandoverStateAuthority HandoverState = iota
	HandoverStateMirror
	HandoverStatePromoting
	HandoverStateDemoting
)

type BorderBand uint8

const (
	BorderBandInterior BorderBand = iota
	BorderBandHysteresis
	BorderBandHandover
)

type MeshForwardHeader struct {
	SourceZoneID   uint32
	TargetZoneID   uint32
	AuthorityEpoch uint32
}

func (h MeshForwardHeader) Encode() [MeshForwardHeaderSize]byte {
	var out [MeshForwardHeaderSize]byte
	binary.LittleEndian.PutUint32(out[0:4], h.SourceZoneID)
	binary.LittleEndian.PutUint32(out[4:8], h.TargetZoneID)
	binary.LittleEndian.PutUint32(out[8:12], h.AuthorityEpoch)
	return out
}

func (h MeshForwardHeader) AppendTo(dst []byte) []byte {
	raw := h.Encode()
	return append(dst, raw[:]...)
}

func EncodeMeshForwardHeader(h MeshForwardHeader) []byte {
	raw := h.Encode()
	return raw[:]
}

func DecodeMeshForwardHeader(raw []byte) (MeshForwardHeader, error) {
	if len(raw) < MeshForwardHeaderSize {
		return MeshForwardHeader{}, ErrTruncatedMeshHeader
	}
	return MeshForwardHeader{
		SourceZoneID:   binary.LittleEndian.Uint32(raw[0:4]),
		TargetZoneID:   binary.LittleEndian.Uint32(raw[4:8]),
		AuthorityEpoch: binary.LittleEndian.Uint32(raw[8:12]),
	}, nil
}

func UnpackMeshForwardPacket(packet []byte) (MeshForwardHeader, []byte, error) {
	hdr, err := DecodeMeshForwardHeader(packet)
	if err != nil {
		return MeshForwardHeader{}, nil, err
	}
	payload := packet[MeshForwardHeaderSize:len(packet):len(packet)]
	return hdr, payload, nil
}

type ZoneBounds struct {
	ZoneID         uint32
	MinX           float64
	MaxX           float64
	MinY           float64
	MaxY           float64
	ServerID       uint32
	Address        string
	AuthorityEpoch uint32
	State          HandoverState
}

func (b ZoneBounds) Contains(x, y float64) bool {
	return x >= b.MinX && x <= b.MaxX && y >= b.MinY && y <= b.MaxY
}

func (b ZoneBounds) SignedBorderDistanceCm(x, y float64) float64 {
	dx := math.Max(b.MinX-x, x-b.MaxX)
	dy := math.Max(b.MinY-y, y-b.MaxY)
	if dx > 0 && dy > 0 {
		return math.Hypot(dx, dy)
	}
	return math.Max(dx, dy)
}

type ZoneRouter struct {
	mu           sync.RWMutex
	hysteresisCm float64
	zones        map[uint32]ZoneBounds
}

func NewZoneRouter(hysteresisCm float64) *ZoneRouter {
	if hysteresisCm <= 0 || math.IsNaN(hysteresisCm) || math.IsInf(hysteresisCm, 0) {
		hysteresisCm = DefaultBoundaryHysteresisCm
	}
	return &ZoneRouter{
		hysteresisCm: hysteresisCm,
		zones:        make(map[uint32]ZoneBounds),
	}
}

func (r *ZoneRouter) HysteresisCm() float64 {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.hysteresisCm
}

func (r *ZoneRouter) UpsertZone(bounds ZoneBounds) error {
	if bounds.ZoneID == 0 || !(bounds.MinX < bounds.MaxX) || !(bounds.MinY < bounds.MaxY) {
		return ErrInvalidZoneBounds
	}
	if bounds.Address != "" {
		if err := ValidAddress(bounds.Address); err != nil {
			return ErrInvalid
		}
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if prev, exists := r.zones[bounds.ZoneID]; exists {
		if bounds.AuthorityEpoch < prev.AuthorityEpoch {
			return ErrStaleAuthorityEpoch
		}
		if bounds.AuthorityEpoch == 0 {
			bounds.AuthorityEpoch = prev.AuthorityEpoch
		}
	} else if bounds.AuthorityEpoch == 0 {
		bounds.AuthorityEpoch = 1
	}
	r.zones[bounds.ZoneID] = bounds
	return nil
}

func (r *ZoneRouter) Zone(zoneID uint32) (ZoneBounds, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	z, ok := r.zones[zoneID]
	return z, ok
}

func (r *ZoneRouter) Zones() []ZoneBounds {
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := make([]ZoneBounds, 0, len(r.zones))
	for _, z := range r.zones {
		out = append(out, z)
	}
	slices.SortFunc(out, func(a, b ZoneBounds) int {
		if a.ZoneID < b.ZoneID {
			return -1
		}
		if a.ZoneID > b.ZoneID {
			return 1
		}
		return 0
	})
	return out
}

// Retain current zone authority within [-200m, +200m] so border jitter never triggers handover flapping.
func (r *ZoneRouter) ClassifyBorderBand(currentZoneID uint32, x, y float64) (BorderBand, uint32, error) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	curr, ok := r.zones[currentZoneID]
	if !ok {
		return BorderBandInterior, 0, ErrUnknownZone
	}
	dist := curr.SignedBorderDistanceCm(x, y)
	if dist < -r.hysteresisCm {
		return BorderBandInterior, currentZoneID, nil
	}
	neighborID := r.nearestNeighborLocked(currentZoneID, x, y)
	if dist <= r.hysteresisCm {
		if neighborID == 0 {
			neighborID = currentZoneID
		}
		return BorderBandHysteresis, neighborID, nil
	}
	if neighborID == 0 {
		return BorderBandHandover, currentZoneID, nil
	}
	return BorderBandHandover, neighborID, nil
}

func (r *ZoneRouter) ResolveAuthorityZone(currentZoneID uint32, x, y float64) (uint32, BorderBand, error) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	if currentZoneID == 0 {
		id := r.containingOrNearestLocked(0, x, y)
		if id == 0 {
			return 0, BorderBandInterior, ErrUnknownZone
		}
		return id, BorderBandInterior, nil
	}
	curr, ok := r.zones[currentZoneID]
	if !ok {
		return 0, BorderBandInterior, ErrUnknownZone
	}
	dist := curr.SignedBorderDistanceCm(x, y)
	if dist < -r.hysteresisCm {
		return currentZoneID, BorderBandInterior, nil
	}
	if dist <= r.hysteresisCm {
		return currentZoneID, BorderBandHysteresis, nil
	}
	targetID := r.containingOrNearestLocked(currentZoneID, x, y)
	if targetID == 0 {
		return currentZoneID, BorderBandHysteresis, nil
	}
	return targetID, BorderBandHandover, nil
}

func (r *ZoneRouter) PromoteZoneAuthority(
	zoneID uint32, newServerID uint32, address string, newEpoch uint32,
) (ZoneBounds, error) {
	if newServerID == StaticServerID {
		return ZoneBounds{}, ErrInvalid
	}
	if address != "" {
		if err := ValidAddress(address); err != nil {
			return ZoneBounds{}, ErrInvalid
		}
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	curr, ok := r.zones[zoneID]
	if !ok {
		return ZoneBounds{}, ErrUnknownZone
	}
	if newEpoch <= curr.AuthorityEpoch {
		return curr, ErrStaleAuthorityEpoch
	}
	curr.ServerID = newServerID
	if address != "" {
		curr.Address = address
	}
	curr.AuthorityEpoch = newEpoch
	curr.State = HandoverStateAuthority
	r.zones[zoneID] = curr
	return curr, nil
}

func (r *ZoneRouter) ValidateForwardHeader(hdr MeshForwardHeader) error {
	if hdr.SourceZoneID == 0 || hdr.TargetZoneID == 0 || hdr.AuthorityEpoch == 0 {
		return ErrInvalid
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	target, ok := r.zones[hdr.TargetZoneID]
	if !ok {
		return ErrUnknownZone
	}
	if hdr.AuthorityEpoch < target.AuthorityEpoch {
		return ErrStaleAuthorityEpoch
	}
	return nil
}

func (r *ZoneRouter) nearestNeighborLocked(excludeZoneID uint32, x, y float64) uint32 {
	var bestID uint32
	bestDist := math.Inf(1)
	for id, z := range r.zones {
		if id == excludeZoneID {
			continue
		}
		d := z.SignedBorderDistanceCm(x, y)
		if d < bestDist || (d == bestDist && (bestID == 0 || id < bestID)) {
			bestDist = d
			bestID = id
		}
	}
	return bestID
}

func (r *ZoneRouter) containingOrNearestLocked(excludeZoneID uint32, x, y float64) uint32 {
	var containingID uint32
	for id, z := range r.zones {
		if id == excludeZoneID {
			continue
		}
		if z.Contains(x, y) && (containingID == 0 || id < containingID) {
			containingID = id
		}
	}
	if containingID != 0 {
		return containingID
	}
	return r.nearestNeighborLocked(excludeZoneID, x, y)
}
