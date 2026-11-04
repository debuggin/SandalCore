from __future__ import annotations
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "re-workbench"))

from re_workbench.bit_oracle import BitCursorStep, evaluate_bunch_bits, verify_corpus_zero_residue
from re_workbench.gap_correlator import CorrelatedGap, build_cleanroom_test_vector, correlate_gap
from re_workbench.reconcile_3way import P0_UNDECODED_STRUCTS, reconcile_3way
from re_workbench.spec_generator import (
    SchemaGapReport,
    render_reconciliation_summary_markdown,
    render_struct_spec_markdown,
)
from re_workbench.static_rizin_usmap import DEFAULT_PE32_PLUS_BASE, StructNetTraits, normalize_rva


class _PayloadGap(CorrelatedGap):
    sample_hex: str = "aabbccdd"


def test_end_to_end_re_workbench_spec_generation() -> None:
    assert normalize_rva(DEFAULT_PE32_PLUS_BASE + 0x1200) == 0x1200
    trait = StructNetTraits("FEssentialCitizenshipWindowData", 0x1200, True)
    records = reconcile_3way({trait.struct_name, *P0_UNDECODED_STRUCTS}, {"FRepMovement": 7}, {"FVector_NetQuantize"})
    matched = [r.struct_name for r in records if r.status == "IMPLEMENTED"]
    missing = [r.struct_name for r in records if r.status == "MISSING_CODEC"]
    report = SchemaGapReport(matched=matched, missing_codec=missing, runtime_only=0, total_p0_structs=len(records))
    base_gap = correlate_gap(trait.struct_name, 4, 0, 12, 44)
    vec = build_cleanroom_test_vector(trait.struct_name, b"\xaa\xbb\xcc\xdd", base_gap.bit_delta)
    assert vec.payload_hex == "aabbccdd" and base_gap.bit_delta == 32
    gap = _PayloadGap(base_gap.struct_name, base_gap.channel_index, base_gap.expected_bits, base_gap.observed_bits, base_gap.bit_delta)
    struct_md = render_struct_spec_markdown(gap)
    summary_md = render_reconciliation_summary_markdown(report, [gap])
    assert "FEssentialCitizenshipWindowData" in struct_md and "aabbccdd" in struct_md
    assert "# Clean-Room Wire Protocol Specification Summary" in summary_md
    assert "FEssentialCitizenshipWindowData" in summary_md and "32" in summary_md and "aabbccdd" in summary_md


def test_bit_oracle_corpus_and_usmap_filter() -> None:
    ok_res = evaluate_bunch_bits(1, 0, 32, [BitCursorStep(1, "CitizenshipWindow", 32, 32)])
    diff_res = evaluate_bunch_bits(2, 0, 32, [BitCursorStep(1, "CitizenshipWindow", 32, 24)])
    trail_res = evaluate_bunch_bits(3, 0, 40, [BitCursorStep(1, "CitizenshipWindow", 32, 32)])
    assert verify_corpus_zero_residue([ok_res]) is True
    assert diff_res.first_divergent_bit is not None and verify_corpus_zero_residue([ok_res, diff_res]) is False
    assert trail_res.leftover_bits > 0 and verify_corpus_zero_residue([ok_res, trail_res]) is False


class ReWorkbenchTest(unittest.TestCase):
    def test_end_to_end_re_workbench_spec_generation(self) -> None:
        test_end_to_end_re_workbench_spec_generation()

    def test_bit_oracle_corpus_and_usmap_filter(self) -> None:
        test_bit_oracle_corpus_and_usmap_filter()


if __name__ == "__main__":
    unittest.main()

