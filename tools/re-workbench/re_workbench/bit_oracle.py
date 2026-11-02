from __future__ import annotations
from collections.abc import Sequence
from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class BitCursorStep:
    handle: int
    field_name: str
    expected_bits: int
    actual_bits: int
    ret_ok: bool = True

@dataclass(frozen=True, slots=True)
class BitOracleResult:
    packet_id: int
    bunch_id: int
    total_bits: int
    consumed_bits: int
    leftover_bits: int
    is_error: bool
    first_divergent_bit: int | None
    first_failed_handle: int | None

def evaluate_bunch_bits(
    packet_id: int, bunch_id: int, total_bits: int, steps: Sequence[BitCursorStep]
) -> BitOracleResult:
    consumed_bits = 0
    first_divergent_bit: int | None = None
    first_failed_handle: int | None = None
    for step in steps:
        if first_failed_handle is None and (
            not step.ret_ok or step.actual_bits != step.expected_bits or step.actual_bits < 0
        ):
            first_divergent_bit = consumed_bits
            first_failed_handle = step.handle
        consumed_bits += step.actual_bits
    leftover_bits = total_bits - consumed_bits
    if first_divergent_bit is None and leftover_bits != 0:
        first_divergent_bit = consumed_bits
    is_error = (first_failed_handle is not None) or (leftover_bits != 0) or (total_bits < 0)
    return BitOracleResult(
        packet_id, bunch_id, total_bits, consumed_bits, leftover_bits, is_error, first_divergent_bit, first_failed_handle
    )

def verify_corpus_zero_residue(results: Sequence[BitOracleResult]) -> bool:
    return bool(results) and all((not r.is_error and r.leftover_bits == 0) for r in results)
