from __future__ import annotations
from contextlib import contextmanager
from pathlib import Path
import re
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "re-workbench"))

from re_workbench.bit_oracle import BitCursorStep, evaluate_bunch_bits, verify_corpus_zero_residue
from re_workbench.gap_correlator import build_cleanroom_test_vector, correlate_gap
from re_workbench.reconcile_3way import P0_UNDECODED_STRUCTS, reconcile_3way


@contextmanager
def _assert_raises_match(exc_type: type[BaseException], match: str):
    try:
        yield
    except exc_type as exc:
        if not re.search(match, str(exc)):
            raise AssertionError(f"Expected {exc!r} to match {match!r}") from exc
    else:
        raise AssertionError(f"Expected {exc_type.__name__} to be raised")


def test_bit_oracle_zero_residue_and_divergence() -> None:
    steps = [BitCursorStep(1, "bHidden", 1, 1), BitCursorStep(2, "RepMovement", 112, 112)]
    res = evaluate_bunch_bits(10, 1, 113, steps)
    assert not res.is_error
    assert res.leftover_bits == 0 and res.first_divergent_bit is None
    assert verify_corpus_zero_residue([res]) is True

    bad_steps = [BitCursorStep(1, "bHidden", 1, 1), BitCursorStep(2, "RepMovement", 32, 24)]
    res_bad = evaluate_bunch_bits(11, 1, 33, bad_steps)
    assert res_bad.is_error is True
    assert res_bad.first_divergent_bit == 1 and res_bad.first_failed_handle == 2
    assert res_bad.leftover_bits == 8 and verify_corpus_zero_residue([res, res_bad]) is False


def test_reconcile_3way_and_gap_correlator() -> None:
    assert len(P0_UNDECODED_STRUCTS) == 12
    records = reconcile_3way(set(P0_UNDECODED_STRUCTS), {"FRepMovement": 42}, {"FVector_NetQuantize"})
    assert len(records) == 12
    by_name = {r.struct_name: r for r in records}
    rep_mov = by_name["FRepMovement"]
    assert rep_mov.priority_id == "P0-01" and rep_mov.status == "MISSING_CODEC"
    assert rep_mov.observed_runtime_calls == 42
    assert by_name["FVector_NetQuantize"].status == "IMPLEMENTED"

    assert correlate_gap("FRepMovement", 3, 112, 16, 128).bit_delta == 0
    with _assert_raises_match(ValueError, "TOOL-9"):
        correlate_gap("FRepMovement", 3, 112, 128, 16)
    assert build_cleanroom_test_vector("FRepMovement", b"\x01\x02", 16).payload_hex == "0102"
    with _assert_raises_match(ValueError, "CLEANROOM-1"):
        build_cleanroom_test_vector("sub_140001000", b"\x01\x02", 16)


class BitOracleTest(unittest.TestCase):
    def test_bit_oracle_zero_residue_and_divergence(self) -> None:
        test_bit_oracle_zero_residue_and_divergence()

    def test_reconcile_3way_and_gap_correlator(self) -> None:
        test_reconcile_3way_and_gap_correlator()


if __name__ == "__main__":
    unittest.main()

