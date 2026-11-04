from __future__ import annotations
from dataclasses import dataclass
import re

DEFAULT_PE32_PLUS_BASE: int = 0x140 << 24
FORBIDDEN_SYMBOL_RE = re.compile(r"\b(?:sub_|qword_|dword_|__fastcall)\w*")


def normalize_rva(va: int, image_base: int = DEFAULT_PE32_PLUS_BASE) -> int:
    if image_base <= 0 or va < image_base:
        raise ValueError("virtual address precedes PE32+ image_base")
    rva = va - image_base
    if rva > 0x7FFF_FFFF:
        raise ValueError("RVA exceeds 2 GiB PE32+ module span")
    return rva


@dataclass(frozen=True, slots=True)
class StructNetTraits:
    struct_name: str
    vtable_rva: int
    with_net_serializer: bool
    with_net_shared_serialization: bool = False

@dataclass(frozen=True, slots=True)
class UsmapPropertyEntry:
    owner_type: str
    prop_name: str
    rep_index: int
    element_size: int
    prop_type: str

@dataclass(frozen=True, slots=True)
class BuildSchemaCatalog:
    exec_sha256: str
    pdb_guid: str
    engine_net_ver: int
    game_net_ver: int
    structs: tuple[StructNetTraits, ...] = ()
    properties: tuple[UsmapPropertyEntry, ...] = ()

    def validate_cleanroom(self) -> None:
        if len(self.exec_sha256) != 64 or not self.pdb_guid:
            raise ValueError("invalid binary manifest pin")
        for s in self.structs:
            if s.vtable_rva < 0 or FORBIDDEN_SYMBOL_RE.search(s.struct_name):
                raise ValueError(f"clean-room violation in struct {s.struct_name}")
        for p in self.properties:
            if p.rep_index < 0 or p.element_size <= 0 or FORBIDDEN_SYMBOL_RE.search(p.prop_name):
                raise ValueError(f"invalid usmap property {p.owner_type}.{p.prop_name}")


def test_static_rizin_usmap_normalize_rva() -> None:
    base = DEFAULT_PE32_PLUS_BASE
    assert normalize_rva(base + 0x24A0, base) == 0x24A0
    for bad_va in (base - 1, base + 0x8000_0000):
        try:
            normalize_rva(bad_va, base)
            raise AssertionError("expected bounds ValueError")
        except ValueError:
            pass


def test_static_rizin_usmap_catalog_and_traits() -> None:
    trait = StructNetTraits("FEssentialCitizenshipWindowData", 0x1A400, True, False)
    prop = UsmapPropertyEntry("AAoCPlayerController", "CitizenshipWindow", 42, 24, "NetSerializeStruct")
    cat = BuildSchemaCatalog("a" * 64, "11223344-5566-7788-99aa-bbccddee0011:1", 32, 104, (trait,), (prop,))
    cat.validate_cleanroom()
    assert cat.structs[0].with_net_serializer and cat.properties[0].rep_index == 42
