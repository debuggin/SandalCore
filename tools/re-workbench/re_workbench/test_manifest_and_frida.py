from __future__ import annotations
import hashlib
import struct
import pytest

try:
    from .frida_session import HOOK_BUDGET_NS, BitTraceRecord, FridaWorkbenchSession, check_hook_latency_ns, decode_ring_slot
    from .manifest_verifier import ManifestPin, PeRsdsInfo, parse_pe_rsds, verify_pe_rsds_manifest
except ImportError:
    from frida_session import HOOK_BUDGET_NS, BitTraceRecord, FridaWorkbenchSession, check_hook_latency_ns, decode_ring_slot  # type: ignore[no-redef]
    from manifest_verifier import ManifestPin, PeRsdsInfo, parse_pe_rsds, verify_pe_rsds_manifest  # type: ignore[no-redef]


def _make_synthetic_pe32_plus() -> bytes:
    buf = bytearray(0x120)
    buf[0:2] = b"MZ"
    struct.pack_into("<I", buf, 0x3C, 0x40)
    buf[0x40:0x44] = b"PE\x00\x00"
    struct.pack_into("<H", buf, 0x40 + 24, 0x020B)
    rsds = b"RSDS" + bytes.fromhex("00112233445566778899AABBCCDDEEFF") + struct.pack("<I", 2)
    buf[0x80 : 0x80 + len(rsds)] = rsds
    return bytes(buf)


def test_pe_rsds_manifest_and_preflight() -> None:
    pe_bytes = _make_synthetic_pe32_plus()
    guid = "00112233445566778899AABBCCDDEEFF"
    digest = hashlib.sha256(pe_bytes).hexdigest()
    info = parse_pe_rsds(pe_bytes)
    assert info == PeRsdsInfo(digest, guid, 2, True)
    pin = ManifestPin(digest, guid, 2)
    assert verify_pe_rsds_manifest(pe_bytes, pin) == info
    assert FridaWorkbenchSession(pin).preflight_verify(pe_bytes) == info
    with pytest.raises(ValueError, match="TOOL-1"):
        verify_pe_rsds_manifest(pe_bytes, ManifestPin(digest, "FF" * 16, 2))
    with pytest.raises(ValueError, match="TOOL-1"):
        verify_pe_rsds_manifest(pe_bytes, ManifestPin("00" * 32, guid, 2))


def test_ring_slot_decode_and_hook_latency() -> None:
    slot = struct.pack("<QIIiiII", 9000, 0xCAFEBABE, 3, 16, 40, 1, 0)
    rec = decode_ring_slot(slot)
    assert rec == BitTraceRecord(9000, 0xCAFEBABE, 3, 16, 40, 1) and rec.bits_consumed == 24
    assert HOOK_BUDGET_NS == 150.0 and check_hook_latency_ns(120.0) is True and check_hook_latency_ns(180.0) is False
    with pytest.raises(ValueError, match="TOOL-9"):
        decode_ring_slot(struct.pack("<QIIiiII", 9000, 1, 0, 40, 16, 1, 0))
