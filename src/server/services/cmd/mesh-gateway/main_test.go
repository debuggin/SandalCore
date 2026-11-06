package main

import (
	"bytes"
	"context"
	"errors"
	"net"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"aocrop/services/internal/mesh"
)

func buildFramedMeshPacket(srcZone, dstZone, epoch uint32, payload []byte) []byte {
	hdr := mesh.EncodeMeshForwardHeader(mesh.MeshForwardHeader{
		SourceZoneID:   srcZone,
		TargetZoneID:   dstZone,
		AuthorityEpoch: epoch,
	})
	out := make([]byte, 0, mesh.MeshForwardHeaderSize+len(payload))
	out = append(out, hdr[:]...)
	out = append(out, payload...)
	return out
}

func TestGatewaySeedDefaultZonesAndRouteForwardPacket(t *testing.T) {
	reg := mesh.NewRegistry(nil, 2*time.Second, "127.0.0.1:7777")
	router := mesh.NewZoneRouter(mesh.DefaultBoundaryHysteresisCm)
	if err := seedDefaultZones(router); err != nil {
		t.Fatalf("seedDefaultZones() error = %v", err)
	}
	gateway, err := newGateway(reg, router, "mesh-gateway-test")
	if err != nil {
		t.Fatalf("newGateway() error = %v", err)
	}

	raw := buildFramedMeshPacket(1, 2, 1, []byte{0xDE, 0xAD, 0xBE, 0xEF})

	hdr, nextZone, payload, err := gateway.RouteForwardPacket(1, -50000.0, 0.0, raw)
	if err != nil || nextZone != 1 || hdr.TargetZoneID != 2 || !bytes.Equal(payload, []byte{0xDE, 0xAD, 0xBE, 0xEF}) {
		t.Fatalf("interior RouteForwardPacket = (%+v, %d, %x, %v)", hdr, nextZone, payload, err)
	}

	_, hZone, _, err := gateway.RouteForwardPacket(1, 15000.0, 0.0, raw)
	if err != nil || hZone != 1 {
		t.Fatalf("hysteresis RouteForwardPacket = (zone=%d, err=%v), want (1, nil)", hZone, err)
	}

	_, crossZone, crossPayload, err := gateway.RouteForwardPacket(1, 25000.0, 0.0, raw)
	if err != nil || crossZone != 2 || !bytes.Equal(crossPayload, []byte{0xDE, 0xAD, 0xBE, 0xEF}) {
		t.Fatalf("cross-border RouteForwardPacket = (zone=%d, payload=%x, err=%v)", crossZone, crossPayload, err)
	}

	if _, _, _, err := gateway.RouteForwardPacket(99, -500000.0, 0.0, raw); !errors.Is(err, mesh.ErrUnknownZone) {
		t.Fatalf("unmapped zone RouteForwardPacket = %v, want ErrUnknownZone", err)
	}
}

func TestGatewayRouteForwardPacketValidationErrors(t *testing.T) {
	gateway, err := newGateway(nil, nil, "mesh-gateway-auto")
	if err != nil || len(gateway.router.Zones()) != 2 {
		t.Fatalf("newGateway(nil, nil) = (%v, zones=%d), want (nil, 2)", err, len(gateway.router.Zones()))
	}

	if _, _, _, err := gateway.RouteForwardPacket(1, -50000.0, 0.0, []byte{1, 2, 3}); !errors.Is(err, mesh.ErrTruncatedMeshHeader) {
		t.Fatalf("truncated packet error = %v, want ErrTruncatedMeshHeader", err)
	}
	stalePkt := buildFramedMeshPacket(1, 2, 0, []byte{0x01})
	if _, _, _, err := gateway.RouteForwardPacket(1, -50000.0, 0.0, stalePkt); !errors.Is(err, mesh.ErrInvalid) {
		t.Fatalf("epoch 0 packet error = %v, want ErrInvalid", err)
	}
	if _, err := gateway.router.PromoteZoneAuthority(2, 2, "127.0.0.1:7778", 3); err != nil {
		t.Fatalf("PromoteZoneAuthority = %v", err)
	}
	epoch1Pkt := buildFramedMeshPacket(1, 2, 1, []byte{0x02})
	if _, _, _, err := gateway.RouteForwardPacket(1, 25000.0, 0.0, epoch1Pkt); !errors.Is(err, mesh.ErrStaleAuthorityEpoch) {
		t.Fatalf("stale epoch packet error = %v, want ErrStaleAuthorityEpoch", err)
	}
}

func TestGatewayHealthProbesAndStaticWorldCheck(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("net.Listen error = %v", err)
	}
	defer ln.Close()

	reg := mesh.NewRegistry(nil, 2*time.Second, ln.Addr().String())
	router := mesh.NewZoneRouter(mesh.DefaultBoundaryHysteresisCm)
	if err := seedDefaultZones(router); err != nil {
		t.Fatalf("seedDefaultZones error = %v", err)
	}
	gateway, err := newGateway(reg, router, "mesh-gateway-probe")
	if err != nil {
		t.Fatalf("newGateway error = %v", err)
	}
	h := gateway.Handler()

	for _, path := range []string{"/livez", "/readyz", "/healthz", "/metrics"} {
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, path, nil))
		if rec.Code != http.StatusOK {
			t.Fatalf("GET %s status = %d, want 200", path, rec.Code)
		}
	}

	gateway.readiness.SetReady(false, true)
	for _, path := range []string{"/readyz", "/healthz"} {
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, path, nil))
		if rec.Code != http.StatusServiceUnavailable {
			t.Fatalf("not-ready GET %s status = %d, want 503", path, rec.Code)
		}
	}
}

func TestGatewayResolveProbeAddrAndRunWithContext(t *testing.T) {
	if err := checkStaticWorld(""); err != nil {
		t.Fatalf("checkStaticWorld(\"\") = %v, want nil", err)
	}
	if err := checkStaticWorld("0.0.0.0:7777"); err == nil {
		t.Fatal("checkStaticWorld(\"0.0.0.0:7777\") = nil, want error")
	}

	t.Setenv("AOC_MESH_GATEWAY_HTTP_BIND_ADDR", "")
	t.Setenv("SANDAL_MESH_GATEWAY_HTTP_BIND_ADDR", "127.0.0.1:17778")
	if got := resolveProbeAddr(); got != "127.0.0.1:17778" {
		t.Fatalf("resolveProbeAddr() = %q, want 127.0.0.1:17778", got)
	}
	t.Setenv("AOC_MESH_GATEWAY_HTTP_BIND_ADDR", "127.0.0.1:17777")
	if got := resolveProbeAddr(); got != "127.0.0.1:17777" {
		t.Fatalf("resolveProbeAddr() = %q, want 127.0.0.1:17777", got)
	}

	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := runWithContext(ctx, "127.0.0.1:0"); err != nil {
		t.Fatalf("runWithContext canceled error = %v", err)
	}
}
