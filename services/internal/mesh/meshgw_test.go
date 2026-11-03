// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package mesh

import (
	"bytes"
	"errors"
	"sync"
	"testing"
	"time"
)

func TestMeshForwardHeader12ByteRoundTripAndLittleEndianParity(t *testing.T) {
	in := MeshForwardHeader{
		SourceZoneID:   0x01020304,
		TargetZoneID:   0x05060708,
		AuthorityEpoch: 0x090A0B0C,
	}
	encoded := EncodeMeshForwardHeader(in)
	if len(encoded) != MeshForwardHeaderSize {
		t.Fatalf("len(EncodeMeshForwardHeader) = %d, want %d", len(encoded), MeshForwardHeaderSize)
	}
	wantBytes := []byte{
		0x04, 0x03, 0x02, 0x01,
		0x08, 0x07, 0x06, 0x05,
		0x0C, 0x0B, 0x0A, 0x09,
	}
	if !bytes.Equal(encoded, wantBytes) {
		t.Fatalf("encoded bytes = %x, want little-endian %x", encoded, wantBytes)
	}
	decoded, err := DecodeMeshForwardHeader(encoded)
	if err != nil {
		t.Fatalf("DecodeMeshForwardHeader failed: %v", err)
	}
	if decoded != in {
		t.Fatalf("decoded = %+v, want %+v", decoded, in)
	}
	packet := in.AppendTo([]byte("prefix"))
	if len(packet) != len("prefix")+MeshForwardHeaderSize {
		t.Fatalf("AppendTo len = %d, want %d", len(packet), len("prefix")+MeshForwardHeaderSize)
	}
}

func TestMeshForwardHeaderTruncationAndPacketUnpack(t *testing.T) {
	for n := 0; n < MeshForwardHeaderSize; n++ {
		raw := make([]byte, n)
		if _, err := DecodeMeshForwardHeader(raw); !errors.Is(err, ErrTruncatedMeshHeader) {
			t.Fatalf("DecodeMeshForwardHeader(%d bytes) = %v, want ErrTruncatedMeshHeader", n, err)
		}
		if _, _, err := UnpackMeshForwardPacket(raw); !errors.Is(err, ErrTruncatedMeshHeader) {
			t.Fatalf("UnpackMeshForwardPacket(%d bytes) = %v, want ErrTruncatedMeshHeader", n, err)
		}
	}
	hdr := MeshForwardHeader{SourceZoneID: 101, TargetZoneID: 102, AuthorityEpoch: 7}
	wire := append(EncodeMeshForwardHeader(hdr), []byte{0xDE, 0xAD, 0xBE, 0xEF}...)
	gotHdr, payload, err := UnpackMeshForwardPacket(wire)
	if err != nil {
		t.Fatalf("UnpackMeshForwardPacket failed: %v", err)
	}
	if gotHdr != hdr || !bytes.Equal(payload, []byte{0xDE, 0xAD, 0xBE, 0xEF}) {
		t.Fatalf("UnpackMeshForwardPacket = (%+v, %x), want (%+v, deadbeef)", gotHdr, payload, hdr)
	}
	if cap(payload) != len(payload) {
		t.Fatalf("payload cap = %d, want 3-index capped %d", cap(payload), len(payload))
	}
}

func newTwoZoneRouter(t *testing.T) *ZoneRouter {
	t.Helper()
	r := NewZoneRouter(DefaultBoundaryHysteresisCm)
	west := ZoneBounds{
		ZoneID:         1,
		MinX:           -200000.0,
		MaxX:           0.0,
		MinY:           -100000.0,
		MaxY:           100000.0,
		ServerID:       11,
		Address:        "127.0.0.1:7777",
		AuthorityEpoch: 1,
		State:          HandoverStateAuthority,
	}
	east := ZoneBounds{
		ZoneID:         2,
		MinX:           0.0,
		MaxX:           200000.0,
		MinY:           -100000.0,
		MaxY:           100000.0,
		ServerID:       12,
		Address:        "127.0.0.1:7778",
		AuthorityEpoch: 1,
		State:          HandoverStateAuthority,
	}
	if err := r.UpsertZone(west); err != nil {
		t.Fatalf("UpsertZone(west): %v", err)
	}
	if err := r.UpsertZone(east); err != nil {
		t.Fatalf("UpsertZone(east): %v", err)
	}
	return r
}

func TestZoneRouter200mHysteresisNoFlapping(t *testing.T) {
	r := newTwoZoneRouter(t)
	if r.HysteresisCm() != 20000.0 {
		t.Fatalf("HysteresisCm() = %v, want 20000.0", r.HysteresisCm())
	}
	authZone := uint32(1)
	sweepWithinBand := []float64{
		-50000.0, -20000.0, -19900.0, -5000.0, 0.0, 5000.0, 19900.0, 20000.0,
		-19900.0, 19900.0, -100.0, 19999.0,
	}
	for _, x := range sweepWithinBand {
		nextZone, band, err := r.ResolveAuthorityZone(authZone, x, 0.0)
		if err != nil {
			t.Fatalf("ResolveAuthorityZone(x=%v): %v", x, err)
		}
		if nextZone != 1 {
			t.Fatalf("at x=%v cm within 200m hysteresis band: authority flapped to zone %d, want zone 1", x, nextZone)
		}
		if x >= -20000.0 && band != BorderBandHysteresis {
			t.Fatalf("at x=%v cm: band = %v, want BorderBandHysteresis", x, band)
		}
	}
	nextZone, band, err := r.ResolveAuthorityZone(authZone, 20001.0, 0.0)
	if err != nil || nextZone != 2 || band != BorderBandHandover {
		t.Fatalf("past +200m (+20001cm): zone=%d band=%v err=%v, want zone=2 BorderBandHandover", nextZone, band, err)
	}
	authZone = nextZone
	for _, x := range []float64{19900.0, 0.0, -19900.0, -20000.0, 15000.0} {
		nextZone, band, err = r.ResolveAuthorityZone(authZone, x, 0.0)
		if err != nil || nextZone != 2 || band != BorderBandHysteresis {
			t.Fatalf("returning to x=%v cm within 200m band: zone=%d band=%v err=%v, want zone=2 BorderBandHysteresis", x, nextZone, band, err)
		}
	}
	nextZone, band, err = r.ResolveAuthorityZone(authZone, -20001.0, 0.0)
	if err != nil || nextZone != 1 || band != BorderBandHandover {
		t.Fatalf("past -200m (-20001cm): zone=%d band=%v err=%v, want zone=1 BorderBandHandover", nextZone, band, err)
	}
}

func TestZoneRouterMonotonicAuthorityEpochFencing(t *testing.T) {
	r := newTwoZoneRouter(t)
	promoted, err := r.PromoteZoneAuthority(1, 21, "127.0.0.1:7780", 5)
	if err != nil {
		t.Fatalf("PromoteZoneAuthority(epoch 5) failed: %v", err)
	}
	if promoted.AuthorityEpoch != 5 || promoted.ServerID != 21 {
		t.Fatalf("promoted = %+v, want epoch=5 server=21", promoted)
	}
	for _, staleEpoch := range []uint32{0, 1, 4, 5} {
		if _, err := r.PromoteZoneAuthority(1, 22, "127.0.0.1:7781", staleEpoch); !errors.Is(err, ErrStaleAuthorityEpoch) {
			t.Fatalf("PromoteZoneAuthority(stale epoch %d) = %v, want ErrStaleAuthorityEpoch", staleEpoch, err)
		}
	}
	if err := r.ValidateForwardHeader(MeshForwardHeader{SourceZoneID: 2, TargetZoneID: 1, AuthorityEpoch: 4}); !errors.Is(err, ErrStaleAuthorityEpoch) {
		t.Fatalf("ValidateForwardHeader(epoch 4 < 5) = %v, want ErrStaleAuthorityEpoch", err)
	}
	if err := r.ValidateForwardHeader(MeshForwardHeader{SourceZoneID: 2, TargetZoneID: 1, AuthorityEpoch: 5}); err != nil {
		t.Fatalf("ValidateForwardHeader(epoch 5) = %v, want nil", err)
	}
}

func TestRegistryLeaseFencedHandoverAndEpochSplitBrainRejection(t *testing.T) {
	reg := NewRegistry(nil, 15*time.Second, "")
	now := time.Date(2026, 11, 3, 12, 0, 0, 0, time.UTC)
	reg.now = func() time.Time { return now }

	if _, err := reg.FenceZoneAuthority(1, 10, 1, HandoverStateAuthority); !errors.Is(err, ErrNoServer) {
		t.Fatalf("FenceZoneAuthority without active lease = %v, want ErrNoServer", err)
	}
	if err := reg.RecordLeaseHeartbeat(10, 1); err != nil {
		t.Fatalf("RecordLeaseHeartbeat(10, 1): %v", err)
	}
	if err := reg.RecordLeaseHeartbeat(11, 1); err != nil {
		t.Fatalf("RecordLeaseHeartbeat(11, 1): %v", err)
	}
	hdr, err := reg.FenceZoneAuthority(1, 10, 2, HandoverStateAuthority)
	if err != nil || hdr.AuthorityEpoch != 2 || hdr.TargetZoneID != 1 {
		t.Fatalf("FenceZoneAuthority(epoch 2) = (%+v, %v), want epoch 2", hdr, err)
	}
	if _, err := reg.FenceZoneAuthority(1, 11, 2, HandoverStateAuthority); !errors.Is(err, ErrStaleAuthorityEpoch) {
		t.Fatalf("split-brain equal epoch 2 = %v, want ErrStaleAuthorityEpoch", err)
	}
	if err := reg.TransitionHandoverState(1, 11, 2, HandoverStatePromoting); !errors.Is(err, ErrStaleAuthorityEpoch) {
		t.Fatalf("split-brain state transition on equal epoch 2 = %v, want ErrStaleAuthorityEpoch", err)
	}
	now = now.Add(16 * time.Second)
	if reg.IsLeaseActive(11) {
		t.Fatalf("IsLeaseActive(11) after 16s = true, want false")
	}
	if _, err := reg.FenceZoneAuthority(1, 11, 3, HandoverStateAuthority); !errors.Is(err, ErrNoServer) {
		t.Fatalf("FenceZoneAuthority with expired lease = %v, want ErrNoServer", err)
	}
	if err := reg.RecordLeaseHeartbeat(11, 3); err != nil {
		t.Fatalf("RecordLeaseHeartbeat(11, 3): %v", err)
	}
	if _, err := reg.FenceZoneAuthority(1, 11, 3, HandoverStateAuthority); err != nil {
		t.Fatalf("FenceZoneAuthority(1, 11, epoch 3) after lease renewal: %v", err)
	}
	owner, epoch, state := reg.ZoneAuthority(1)
	if owner != 11 || epoch != 3 || state != HandoverStateAuthority {
		t.Fatalf("ZoneAuthority(1) = (%d, %d, %v), want (11, 3, Authority)", owner, epoch, state)
	}
}

func TestZoneRouterConcurrentReadWriteSafety(t *testing.T) {
	r := newTwoZoneRouter(t)
	var wg sync.WaitGroup
	for w := 0; w < 4; w++ {
		wg.Add(1)
		go func(worker uint32) {
			defer wg.Done()
			for step := uint32(2); step < 40; step++ {
				epoch := step*10 + worker
				_, _ = r.PromoteZoneAuthority(1, 10+worker, "127.0.0.1:7777", epoch)
				_, _, _ = r.ResolveAuthorityZone(1, -15000.0, 0.0)
				_, _, _ = r.ClassifyBorderBand(1, 15000.0, 0.0)
				_ = r.Zones()
			}
		}(uint32(w))
	}
	for reader := 0; reader < 8; reader++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < 60; i++ {
				_, _, _ = r.ResolveAuthorityZone(1, 19500.0, 0.0)
				_, _ = r.Zone(1)
				_ = r.ValidateForwardHeader(MeshForwardHeader{
					SourceZoneID:   1,
					TargetZoneID:   2,
					AuthorityEpoch: 1,
				})
			}
		}()
	}
	wg.Wait()
	z, ok := r.Zone(1)
	if !ok || z.AuthorityEpoch < 2 {
		t.Fatalf("final Zone(1) = %+v (ok=%v), want monotonic AuthorityEpoch >= 2", z, ok)
	}
}
