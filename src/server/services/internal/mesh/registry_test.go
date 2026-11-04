// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package mesh

import (
	"context"
	"errors"
	"math"
	"testing"
	"time"

	"aocrop/services/internal/db"
	"aocrop/services/internal/dbtest"
)

// setup returns a registry on a fresh schema whose clock the test moves.
func setup(t *testing.T, static string) (*Registry, *time.Time) {
	t.Helper()
	pool := dbtest.Pool(t)
	if err := db.Migrate(context.Background(), pool); err != nil {
		t.Fatal(err)
	}
	s := NewRegistry(pool, 15*time.Second, static)
	now := time.Date(2026, 9, 29, 12, 0, 0, 0, time.UTC)
	s.now = func() time.Time { return now }
	return s, &now
}

func TestRegisterGivesDistinctIDsFromOne(t *testing.T) {
	s, _ := setup(t, "")
	ctx := context.Background()
	a, err := s.Register(ctx, "127.0.0.1:7777", []string{"verra"})
	if err != nil {
		t.Fatal(err)
	}
	b, err := s.Register(ctx, "127.0.0.1:7778", nil)
	if err != nil {
		t.Fatal(err)
	}
	if a < 1 || b < 1 || a == b {
		t.Errorf("server ids %d and %d, want distinct and >= 1", a, b)
	}
}

func TestRegisterRejectsBadInput(t *testing.T) {
	s, _ := setup(t, "")
	for _, tc := range []struct {
		address string
		regions []string
	}{
		{"", nil}, {"no-port", nil}, {"a\x00b:7777", nil}, {"\xff:7777", nil},
		{"127.0.0.1:7777", []string{"ok", "bad\x00region"}}, {"127.0.0.1:7777", []string{"bad\xffregion"}},
	} {
		if _, err := s.Register(context.Background(), tc.address, tc.regions); !errors.Is(err, ErrInvalid) {
			t.Errorf("Register(%q, %q) = %v, want ErrInvalid", tc.address, tc.regions, err)
		}
	}
}

func TestHeartbeatUnknownServer(t *testing.T) {
	s, _ := setup(t, "")
	for _, id := range []uint32{0, 999, math.MaxInt32 + 1, math.MaxUint32} {
		if err := s.Heartbeat(context.Background(), id); !errors.Is(err, ErrUnknownServer) {
			t.Errorf("Heartbeat(%d) = %v, want ErrUnknownServer", id, err)
		}
	}
}

// [Cyb x C O A]

func TestPlaceWithNoServerAndNoStaticWorld(t *testing.T) {
	s, _ := setup(t, "")
	if _, _, err := s.Place(context.Background(), "0123456789abcdef0123456789abcdef"); !errors.Is(err, ErrNoServer) {
		t.Errorf("Place = %v, want ErrNoServer", err)
	}
}

func TestPlaceFallsBackToTheStaticWorld(t *testing.T) {
	s, _ := setup(t, "127.0.0.1:7777")
	id, addr, err := s.Place(context.Background(), "0123456789abcdef0123456789abcdef")
	if err != nil || id != StaticServerID || addr != "127.0.0.1:7777" {
		t.Errorf("Place = %d %q %v, want the static world", id, addr, err)
	}
}

func TestPlacePrefersALiveRegisteredServer(t *testing.T) {
	s, _ := setup(t, "127.0.0.1:7777")
	ctx := context.Background()
	reg, err := s.Register(ctx, "192.0.2.5:7001", nil)
	if err != nil {
		t.Fatal(err)
	}
	id, addr, err := s.Place(ctx, "0123456789abcdef0123456789abcdef")
	if err != nil || id != reg || addr != "192.0.2.5:7001" {
		t.Errorf("Place = %d %q %v, want server %d", id, addr, err, reg)
	}
}

// A server that misses heartbeats for the TTL is not placed; a heartbeat
// brings it back.
func TestPlaceSkipsASilentServerUntilItHeartbeats(t *testing.T) {
	s, now := setup(t, "127.0.0.1:7777")
	ctx := context.Background()
	reg, err := s.Register(ctx, "192.0.2.5:7001", nil)
	if err != nil {
		t.Fatal(err)
	}
	*now = now.Add(16 * time.Second)
	if id, _, err := s.Place(ctx, "0123456789abcdef0123456789abcdef"); err != nil || id != StaticServerID {
		t.Errorf("after 16 s of silence: Place = %d %v, want the static world", id, err)
	}
	if err := s.Heartbeat(ctx, reg); err != nil {
		t.Fatal(err)
	}
	if id, _, err := s.Place(ctx, "0123456789abcdef0123456789abcdef"); err != nil || id != reg {
		t.Errorf("after a heartbeat: Place = %d %v, want %d", id, err, reg)
	}
}

func TestPlacePicksTheMostRecentHeartbeat(t *testing.T) {
	s, now := setup(t, "")
	ctx := context.Background()
	a, err := s.Register(ctx, "192.0.2.5:7001", nil)
	if err != nil {
		t.Fatal(err)
	}
	b, err := s.Register(ctx, "192.0.2.6:7001", nil)
	if err != nil {
		t.Fatal(err)
	}
	*now = now.Add(5 * time.Second)
	if err := s.Heartbeat(ctx, a); err != nil {
		t.Fatal(err)
	}
	if id, _, err := s.Place(ctx, "0123456789abcdef0123456789abcdef"); err != nil || id != a {
		t.Errorf("Place = %d %v, want %d (heartbeat after %d's)", id, err, a, b)
	}
}

const someChar = "0123456789abcdef0123456789abcdef"

// The TTL boundary: silent for less than the TTL is placed, for exactly the
// TTL is not ("silent this long is not placed").
func TestPlaceTTLBoundary(t *testing.T) {
	s, now := setup(t, "127.0.0.1:7777") // ttl is 15 s
	ctx := context.Background()
	reg, err := s.Register(ctx, "192.0.2.5:7001", nil)
	if err != nil {
		t.Fatal(err)
	}
	t0 := *now
	for _, tc := range []struct {
		silent time.Duration
		placed bool
	}{
		{14 * time.Second, true},
		{15*time.Second - time.Microsecond, true},
		{15 * time.Second, false},
		{15*time.Second + time.Microsecond, false},
	} {
		*now = t0.Add(tc.silent)
		id, _, err := s.Place(ctx, someChar)
		if err != nil || (id == reg) != tc.placed {
			t.Errorf("silent for %v: Place = %d %v, want placed=%v", tc.silent, id, err, tc.placed)
		}
	}
}

// Servers with the same last heartbeat: the one registered later wins, so the
// answer does not depend on row order.
func TestPlaceTieGoesToTheNewerRegistration(t *testing.T) {
	s, _ := setup(t, "")
	ctx := context.Background()
	if _, err := s.Register(ctx, "192.0.2.5:7001", nil); err != nil {
		t.Fatal(err)
	}
	b, err := s.Register(ctx, "192.0.2.6:7001", nil)
	if err != nil {
		t.Fatal(err)
	}
	if id, _, err := s.Place(ctx, someChar); err != nil || id != b {
		t.Errorf("Place = %d %v, want %d", id, err, b)
	}
}

// A heartbeat restarts the TTL window from the heartbeat, on the store's clock.
func TestHeartbeatRestartsTheTTLWindow(t *testing.T) {
	s, now := setup(t, "127.0.0.1:7777")
	ctx := context.Background()
	reg, err := s.Register(ctx, "192.0.2.5:7001", nil)
	if err != nil {
		t.Fatal(err)
	}
	*now = now.Add(10 * time.Second)
	if err := s.Heartbeat(ctx, reg); err != nil {
		t.Fatal(err)
	}
	*now = now.Add(10 * time.Second) // 20 s after Register, 10 s after the heartbeat
	if id, _, err := s.Place(ctx, someChar); err != nil || id != reg {
		t.Errorf("10 s after a heartbeat: Place = %d %v, want %d", id, err, reg)
	}
	*now = now.Add(6 * time.Second) // 16 s after the heartbeat
	if id, _, err := s.Place(ctx, someChar); err != nil || id != StaticServerID {
		t.Errorf("16 s after a heartbeat: Place = %d %v, want the static world", id, err)
	}
}

// A database failure is an error, never "unknown server", "no server" or the
// static world: worldd must retry it, not re-register, and the gateway must
// not send a player to the legacy server because the database blinked.
func TestDatabaseFailureIsNotAnAnswer(t *testing.T) {
	s, _ := setup(t, "127.0.0.1:7777")
	ctx := context.Background()
	reg, err := s.Register(ctx, "192.0.2.5:7001", nil)
	if err != nil {
		t.Fatal(err)
	}
	s.pool.Close()
	if err := s.Heartbeat(ctx, reg); err == nil || errors.Is(err, ErrUnknownServer) {
		t.Errorf("Heartbeat on a closed pool = %v, want a database error", err)
	}
	if id, addr, err := s.Place(ctx, someChar); err == nil || errors.Is(err, ErrNoServer) {
		t.Errorf("Place on a closed pool = %d %q %v, want a database error", id, addr, err)
	}
	if _, err := s.Register(ctx, "192.0.2.6:7001", nil); err == nil || errors.Is(err, ErrInvalid) {
		t.Errorf("Register on a closed pool = %v, want a database error", err)
	}
}

// C.Y.B + CoA

func TestHeartbeatTTLIsTheConfiguredOne(t *testing.T) {
	s, _ := setup(t, "")
	s2 := NewStore(s.pool, 20*time.Second, "127.0.0.1:1")
	if s2.HeartbeatTTL() != 20*time.Second {
		t.Errorf("HeartbeatTTL = %v, want 20s", s2.HeartbeatTTL())
	}
	// and Place honours it: 19 s of silence is still live
	s2.now = s.now
	ctx := context.Background()
	reg, err := s2.Register(ctx, "192.0.2.5:7001", nil)
	if err != nil {
		t.Fatal(err)
	}
	t0 := s2.now()
	s2.now = func() time.Time { return t0.Add(19 * time.Second) }
	if id, _, err := s2.Place(ctx, someChar); err != nil || id != reg {
		t.Errorf("19 s of silence with a 20 s TTL: Place = %d %v, want %d", id, err, reg)
	}
}

func TestStaticServerIDIsZero(t *testing.T) {
	if StaticServerID != 0 {
		t.Errorf("StaticServerID = %d; the proto, the schema and worldd all say 0", StaticServerID)
	}
}

// Addresses net.SplitHostPort accepts but a game client cannot connect to,
// among them what worldd would send if it registered its bind address.
func TestRegisterRejectsAddressesTheClientCannotDial(t *testing.T) {
	s, _ := setup(t, "")
	for _, address := range []string{":7777", "host:", ":", "host:abc", "host:0", "host:65536", "a b:7777", "0.0.0.0:7777", "[::]:7777"} {
		if _, err := s.Register(context.Background(), address, nil); !errors.Is(err, ErrInvalid) {
			t.Errorf("Register(%q) = %v, want ErrInvalid", address, err)
		}
	}
}

// ValidAddress is also what cmd/mesh checks AOC_MESH_STATIC_WORLD with.
func TestValidAddress(t *testing.T) {
	for _, address := range []string{
		"127.0.0.1:7777", "192.0.2.5:1", "localhost:7777", "world.example.com:65535",
		"[::1]:7777", "[2001:db8::1]:7777", "0.0.0.1:7777", "aoc_worldd_1:7777", "a-b.c:7777", "example.com.:7777", "World-1.Example.COM:7777",
	} {
		if err := ValidAddress(address); err != nil {
			t.Errorf("ValidAddress(%q) = %v, want nil", address, err)
		}
	}
	for _, address := range []string{
		"", "no-port", ":7777", ":", "host:", "host:abc", "host:0", "host:65536", "host:-1", "host:+1", "host:77 77",
		"0.0.0.0:7777", "[::]:7777", "[::ffff:0.0.0.0]:7777",
		"a b:7777", "a\tb:7777", "a\nb:7777", "a\x7fb:7777", "a\x00b:7777", "\xff:7777",
		"nb\u00a0sp:7777", "zw\u200bsp:7777", "\ufeffhost:7777", "m\u00fcnchen:7777", "a/b:7777", "[fe80::1%eth0]:7777",
		"host:0x50", "host:1_000",
		"nel\u0085x:7777", "c1\u009bx:7777", "em\u2003sp:7777", "lrm\u200ex:7777", "rlo\u202ex:7777",
	} {
		if err := ValidAddress(address); err == nil {
			t.Errorf("ValidAddress(%q) = nil, want an error", address)
		}
	}
}

// Equal heartbeats whose rows are not in id order: still the higher id.
func TestPlaceTieBreakIgnoresRowOrder(t *testing.T) {
	s, now := setup(t, "")
	ctx := context.Background()
	a, _ := s.Register(ctx, "192.0.2.5:7001", nil)
	b, _ := s.Register(ctx, "192.0.2.6:7001", nil)
	*now = now.Add(time.Second)
	// b's row is rewritten first, a's last: a is physically after b.
	if err := s.Heartbeat(ctx, b); err != nil {
		t.Fatal(err)
	}
	if err := s.Heartbeat(ctx, a); err != nil {
		t.Fatal(err)
	}
	if id, _, err := s.Place(ctx, someChar); err != nil || id != b {
		t.Errorf("Place = %d %v, want %d (the higher id of equal heartbeats)", id, err, b)
	}
}

func TestRegistryLeaseHeartbeatAndFenceValidation(t *testing.T) {
	var zeroReg Registry
	if zeroReg.IsLeaseActive(1) || zeroReg.AuthorityEpoch(1) != 0 {
		t.Fatal("zero Registry should report inactive lease and zero epoch")
	}
	if srv, ep, st := zeroReg.ZoneAuthority(1); srv != 0 || ep != 0 || st != HandoverStateMirror {
		t.Fatalf("zero ZoneAuthority = (%d, %d, %d), want (0, 0, Mirror)", srv, ep, st)
	}

	s := NewRegistry(nil, 0, "127.0.0.1:7777")
	if err := s.RecordLeaseHeartbeat(StaticServerID, 1); !errors.Is(err, ErrUnknownServer) {
		t.Fatalf("RecordLeaseHeartbeat(0) = %v, want ErrUnknownServer", err)
	}
	if err := s.RecordLeaseHeartbeat(uint32(math.MaxInt32)+1, 1); !errors.Is(err, ErrUnknownServer) {
		t.Fatalf("RecordLeaseHeartbeat(>MaxInt32) = %v, want ErrUnknownServer", err)
	}
	if err := s.RecordLeaseHeartbeat(10, 5); err != nil || !s.IsLeaseActive(10) {
		t.Fatalf("RecordLeaseHeartbeat(10, 5) = %v, active=%v", err, s.IsLeaseActive(10))
	}
	if err := s.RecordLeaseHeartbeat(10, 4); !errors.Is(err, ErrStaleAuthorityEpoch) {
		t.Fatalf("RecordLeaseHeartbeat stale epoch = %v, want ErrStaleAuthorityEpoch", err)
	}
	for _, tc := range []struct {
		zoneID, srvID, epoch uint32
		state                HandoverState
		want                 error
	}{
		{0, 10, 1, HandoverStateAuthority, ErrInvalid},
		{1, 0, 1, HandoverStateAuthority, ErrInvalid},
		{1, uint32(math.MaxInt32) + 1, 1, HandoverStateAuthority, ErrInvalid},
		{1, 10, 0, HandoverStateAuthority, ErrInvalid},
		{1, 99, 1, HandoverStateAuthority, ErrNoServer},
	} {
		if _, err := s.FenceZoneAuthority(tc.zoneID, tc.srvID, tc.epoch, tc.state); !errors.Is(err, tc.want) {
			t.Fatalf("FenceZoneAuthority(%+v) = %v, want %v", tc, err, tc.want)
		}
	}
}

func TestRegistryHandoverStateLifecycleAndEpochFence(t *testing.T) {
	now := time.Unix(1_700_000_000, 0)
	s := NewRegistry(nil, 5*time.Second, "")
	s.now = func() time.Time { return now }
	if err := s.RecordLeaseHeartbeat(1, 1); err != nil {
		t.Fatalf("RecordLeaseHeartbeat(1, 1) = %v", err)
	}
	if err := s.RecordLeaseHeartbeat(2, 2); err != nil {
		t.Fatalf("RecordLeaseHeartbeat(2, 2) = %v", err)
	}

	hdr1, err := s.FenceZoneAuthority(10, 1, 1, HandoverStateAuthority)
	if err != nil || hdr1.SourceZoneID != 0 || hdr1.TargetZoneID != 10 || hdr1.AuthorityEpoch != 1 {
		t.Fatalf("initial FenceZoneAuthority = %+v, err=%v", hdr1, err)
	}
	if err := s.TransitionHandoverState(10, 1, 1, HandoverStatePromoting); err != nil {
		t.Fatalf("TransitionHandoverState(Promoting) = %v", err)
	}
	if err := s.TransitionHandoverState(10, 1, 0, HandoverStateAuthority); !errors.Is(err, ErrInvalid) {
		t.Fatalf("TransitionHandoverState(epoch=0) = %v, want ErrInvalid", err)
	}
	if err := s.TransitionHandoverState(10, 2, 1, HandoverStateAuthority); !errors.Is(err, ErrStaleAuthorityEpoch) {
		t.Fatalf("non-owner same-epoch transition = %v, want ErrStaleAuthorityEpoch", err)
	}
	hdr2, err := s.FenceZoneAuthority(10, 2, 2, HandoverStateDemoting)
	if err != nil || hdr2.SourceZoneID != 1 || hdr2.AuthorityEpoch != 2 {
		t.Fatalf("second FenceZoneAuthority = %+v, err=%v", hdr2, err)
	}
	if err := s.TransitionHandoverState(10, 2, 2, HandoverStateAuthority); err != nil {
		t.Fatalf("TransitionHandoverState(Authority) = %v", err)
	}
	now = now.Add(6 * time.Second)
	if s.IsLeaseActive(2) {
		t.Fatal("expected server 2 lease to expire after TTL")
	}
	if err := s.TransitionHandoverState(10, 2, 3, HandoverStateMirror); !errors.Is(err, ErrNoServer) {
		t.Fatalf("TransitionHandoverState expired lease = %v, want ErrNoServer", err)
	}
}

func TestZoneRouterUpsertAndPromoteValidation(t *testing.T) {
	r := NewZoneRouter(math.NaN())
	if r.HysteresisCm() != DefaultBoundaryHysteresisCm {
		t.Fatalf("HysteresisCm() = %v, want %v", r.HysteresisCm(), DefaultBoundaryHysteresisCm)
	}
	if err := r.UpsertZone(ZoneBounds{ZoneID: 0, MaxX: 10, MaxY: 10, ServerID: 1, Address: "127.0.0.1:7001"}); !errors.Is(err, ErrInvalidZoneBounds) {
		t.Fatalf("UpsertZone(ZoneID=0) = %v, want ErrInvalidZoneBounds", err)
	}
	if err := r.UpsertZone(ZoneBounds{ZoneID: 1, MinX: 10, MaxX: 10, MaxY: 10, ServerID: 1, Address: "127.0.0.1:7001"}); !errors.Is(err, ErrInvalidZoneBounds) {
		t.Fatalf("UpsertZone(MinX==MaxX) = %v, want ErrInvalidZoneBounds", err)
	}
	if err := r.UpsertZone(ZoneBounds{ZoneID: 1, MaxX: 100, MaxY: 100, ServerID: 1, Address: "0.0.0.0:7001"}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("UpsertZone(unspecified address) = %v, want ErrInvalid", err)
	}
	if err := r.UpsertZone(ZoneBounds{ZoneID: 1, MaxX: 100, MaxY: 100, ServerID: 1, Address: "127.0.0.1:7001", AuthorityEpoch: 3}); err != nil {
		t.Fatalf("UpsertZone valid = %v", err)
	}
	if err := r.UpsertZone(ZoneBounds{ZoneID: 1, MaxX: 100, MaxY: 100, ServerID: 1, Address: "127.0.0.1:7001", AuthorityEpoch: 0}); !errors.Is(err, ErrStaleAuthorityEpoch) {
		t.Fatalf("UpsertZone epoch=0 after epoch=3 = %v, want ErrStaleAuthorityEpoch", err)
	}
	if _, err := r.PromoteZoneAuthority(99, 2, "127.0.0.1:7002", 4); !errors.Is(err, ErrUnknownZone) {
		t.Fatalf("PromoteZoneAuthority unknown = %v, want ErrUnknownZone", err)
	}
	if _, err := r.PromoteZoneAuthority(1, 0, "127.0.0.1:7002", 4); !errors.Is(err, ErrInvalid) {
		t.Fatalf("PromoteZoneAuthority serverID=0 = %v, want ErrInvalid", err)
	}
}

func TestZoneRouterBoundarySweepHysteresisAndForwardHeader(t *testing.T) {
	emptyRouter := NewZoneRouter(0)
	if _, _, err := emptyRouter.ResolveAuthorityZone(0, 0, 0); !errors.Is(err, ErrUnknownZone) {
		t.Fatalf("empty ResolveAuthorityZone = %v, want ErrUnknownZone", err)
	}
	r := newTwoZoneRouter(t)
	if z, band, err := r.ResolveAuthorityZone(0, -50000, 0); err != nil || z != 1 || band != BorderBandInterior {
		t.Fatalf("ResolveAuthorityZone(0, -50000, 0) = (%d, %v, %v)", z, band, err)
	}
	for _, step := range []struct {
		x        float64
		wantZone uint32
		wantBand BorderBand
	}{
		{-25000, 1, BorderBandInterior},
		{-5000, 1, BorderBandHysteresis},
		{15000, 1, BorderBandHysteresis},
		{25000, 2, BorderBandHandover},
	} {
		z, band, err := r.ResolveAuthorityZone(1, step.x, 0)
		if err != nil || z != step.wantZone || band != step.wantBand {
			t.Fatalf("sweep x=%v: got (%d, %v, %v), want (%d, %v, nil)", step.x, z, band, err, step.wantZone, step.wantBand)
		}
	}
	if err := r.ValidateForwardHeader(MeshForwardHeader{SourceZoneID: 0, TargetZoneID: 2, AuthorityEpoch: 1}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("ValidateForwardHeader(zero source) = %v, want ErrInvalid", err)
	}
	if err := r.ValidateForwardHeader(MeshForwardHeader{SourceZoneID: 1, TargetZoneID: 99, AuthorityEpoch: 1}); !errors.Is(err, ErrUnknownZone) {
		t.Fatalf("ValidateForwardHeader(unknown target) = %v, want ErrUnknownZone", err)
	}
}

