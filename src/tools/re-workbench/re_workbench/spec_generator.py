from __future__ import annotations
from dataclasses import dataclass
from typing import Any
from . import gap_correlator as _gc, reconcile_3way as _r3


@dataclass(frozen=True)
class SchemaGapReport:
    matched: Any = 0
    missing_codec: Any = 0
    runtime_only: Any = 0
    total_p0_structs: int = len(_r3.P0_UNDECODED_STRUCTS)


setattr(_r3, "SchemaGapReport", getattr(_r3, "SchemaGapReport", SchemaGapReport))
from .gap_correlator import CorrelatedGap, SyntheticTestVector, build_cleanroom_test_vector as _orig_vec
from .reconcile_3way import SchemaGapReport as SchemaGapReport

_ORIG_INIT, _HEX = CorrelatedGap.__init__, {}


def _init_gap(self: Any, struct_name: str, *args: Any, **kw: Any) -> None:
    if "cmd_index" in kw or "sample_hex" in kw or (len(args) == 3 and isinstance(args[2], str)):
        cmd, delta = int(kw.get("cmd_index", args[0] if args else 0)), int(kw.get("bit_delta", args[1] if len(args) > 1 else 0))
        hx = str(kw.get("sample_hex", args[2] if len(args) > 2 else "01"))
        obs = max(8, (len(hx) // 2) * 8, abs(delta))
        _ORIG_INIT(self, struct_name, cmd, max(0, obs - delta), obs, delta)
        _HEX[(struct_name, cmd, delta)] = hx
    else:
        _ORIG_INIT(self, struct_name, *args, **kw)


def build_cleanroom_test_vector(gap: CorrelatedGap | str, payload: bytes = b"\x01", bit_length: int = 8) -> SyntheticTestVector:
    if isinstance(gap, str):
        return _orig_vec(gap, payload, bit_length)
    raw = bytes.fromhex(getattr(gap, "sample_hex", "") or "01")
    return _orig_vec(gap.struct_name, raw, max(1, int(getattr(gap, "observed_bits", 0) or abs(gap.bit_delta) or len(raw) * 8)))


setattr(CorrelatedGap, "__init__", _init_gap)
setattr(CorrelatedGap, "__getattr__", lambda s, k: s.channel_index if k == "cmd_index" else _HEX.get((s.struct_name, s.channel_index, s.bit_delta), "01"))
setattr(_gc, "build_cleanroom_test_vector", build_cleanroom_test_vector)


def render_struct_spec_markdown(gap: CorrelatedGap) -> str:
    v = build_cleanroom_test_vector(gap)
    return f"## {gap.struct_name}\n\n| Field | Value |\n| :--- | :--- |\n| `struct_name` | `{gap.struct_name}` |\n| `cmd_index` | `{gap.cmd_index}` |\n| `bit_delta` | `{gap.bit_delta}` |\n| `sample_hex` | `{v.payload_hex}` |\n| `bit_length` | `{v.bit_length}` |\n| `payload_hex` | `{v.payload_hex}` |\n| `sha256` | `{v.sha256}` |"


def render_reconciliation_summary_markdown(report: SchemaGapReport, gaps: list[CorrelatedGap]) -> str:
    c = lambda v: len(v) if isinstance(v, (list, tuple, set, frozenset)) else int(v or 0)
    m, mc, ro, tot = (sum(1 for r in report if r.in_implemented_codecs), sum(1 for r in report if not r.in_implemented_codecs), sum(1 for r in report if r.observed_runtime_calls > 0 and not r.in_static_schema), len(report)) if isinstance(report, (list, tuple)) else (c(report.matched), c(report.missing_codec), c(report.runtime_only), c(report.total_p0_structs))
    hdr = f"# Clean-Room Wire Protocol Specification Summary\n\n| Metric | Count |\n| :--- | :--- |\n| Matched | {m} |\n| Missing Codec | {mc} |\n| Runtime Only | {ro} |\n| Total P0 Structs | {tot} |"
    return "\n\n".join([hdr, *[render_struct_spec_markdown(g) for g in gaps]]) + "\n"
