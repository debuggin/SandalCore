from __future__ import annotations
from dataclasses import dataclass
import struct

try:
    from .manifest_verifier import ManifestPin, PeRsdsInfo, verify_pe_rsds_manifest
except ImportError:
    from manifest_verifier import ManifestPin, PeRsdsInfo, verify_pe_rsds_manifest  # type: ignore[no-redef]

HOOK_BUDGET_NS = 150.0
RING_CAPACITY_SLOTS = 65536
SLOT_SIZE_BYTES = 32

@dataclass(frozen=True, slots=True)
class BitTraceRecord:
    qpc_ticks: int
    struct_type_hash: int
    channel_index: int
    bit_pos_before: int
    bit_pos_after: int
    ret_ok: int

    @property
    def bits_consumed(self) -> int:
        return self.bit_pos_after - self.bit_pos_before

def decode_ring_slot(buf: bytes, offset: int = 0) -> BitTraceRecord:
    if offset < 0 or len(buf) - offset < SLOT_SIZE_BYTES:
        raise ValueError("TOOL-9: truncated ring slot buffer")
    qpc, type_hash, chan_idx, before, after, ret_ok, _ = struct.unpack_from("<QIIiiII", buf, offset)
    if before < 0 or after < before:
        raise ValueError("TOOL-9: invalid bit cursor delta")
    return BitTraceRecord(qpc, type_hash, chan_idx, before, after, ret_ok)

def check_hook_latency_ns(elapsed_ns: float, budget_ns: float = HOOK_BUDGET_NS) -> bool:
    return 0.0 <= elapsed_ns <= budget_ns

@dataclass(slots=True)
class FridaWorkbenchSession:
    pin: ManifestPin
    verified_info: PeRsdsInfo | None = None

    def preflight_verify(self, pe_bytes: bytes) -> PeRsdsInfo:
        self.verified_info = verify_pe_rsds_manifest(pe_bytes, self.pin)
        return self.verified_info
