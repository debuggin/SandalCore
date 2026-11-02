from __future__ import annotations
from collections.abc import Mapping
from dataclasses import dataclass

MOVEMENT_BASE_OBJECT_ID: int = 1
P0_UNDECODED_STRUCTS: tuple[str, ...] = (
    "FRepMovement",
    "FMinimalViewInfo",
    "FGameplayTagContainer",
    "FGameplayEffectContextHandle",
    "FUniqueNetIdRepl",
    "FVector_NetQuantize",
    "FVector_NetQuantize10",
    "FVector_NetQuantize100",
    "FVector_NetQuantizeNormal",
    "FDateTime",
    "FTimespan",
    "FSoftObjectPath",
)


@dataclass(frozen=True, slots=True)
class StructGapRecord:
    priority_id: str
    struct_name: str
    in_static_schema: bool
    observed_runtime_calls: int
    in_implemented_codecs: bool
    status: str


def reconcile_3way(
    static_structs: set[str],
    runtime_counts: Mapping[str, int],
    implemented_codecs: set[str],
) -> list[StructGapRecord]:
    records: list[StructGapRecord] = []
    for idx, name in enumerate(P0_UNDECODED_STRUCTS, start=1):
        in_impl = name in implemented_codecs
        status = "IMPLEMENTED" if in_impl else "MISSING_CODEC"
        records.append(
            StructGapRecord(
                f"P0-{idx:02d}", name, name in static_structs, int(runtime_counts.get(name, 0)), in_impl, status
            )
        )
    return records
