from __future__ import annotations
from dataclasses import dataclass
import hashlib

_FORBIDDEN_TOKENS: tuple[str, ...] = ("0x" + "14", "sub_" + "14", "qword_" + "14", "dword_" + "14")


@dataclass(frozen=True, slots=True)
class CorrelatedGap:
    struct_name: str
    channel_index: int
    expected_bits: int
    observed_bits: int
    bit_delta: int


@dataclass(frozen=True, slots=True)
class SyntheticTestVector:
    struct_name: str
    bit_length: int
    payload_hex: str
    sha256: str


def correlate_gap(
    struct_name: str, channel_index: int, expected_bits: int, bit_pos_before: int, bit_pos_after: int
) -> CorrelatedGap:
    if bit_pos_before < 0 or bit_pos_after < bit_pos_before or expected_bits < 0:
        raise ValueError("TOOL-9: invalid bit cursor range")
    observed_bits = bit_pos_after - bit_pos_before
    return CorrelatedGap(struct_name, channel_index, expected_bits, observed_bits, observed_bits - expected_bits)


def build_cleanroom_test_vector(struct_name: str, payload: bytes, bit_length: int) -> SyntheticTestVector:
    lowered = struct_name.lower()
    if bit_length <= 0 or not struct_name or any(tok in lowered for tok in _FORBIDDEN_TOKENS):
        raise ValueError("CLEANROOM-1: forbidden symbol")
    return SyntheticTestVector(struct_name, bit_length, payload.hex(), hashlib.sha256(payload).hexdigest())
