from __future__ import annotations
from dataclasses import dataclass
import struct

TOOL_DIAGNOSTICS: dict[str, str] = {
    "TOOL-1": "Verify PE32+ SHA-256 and RSDS PDB GUID+Age manifest pin before loading schema",
    "TOOL-2": "Normalize virtual addresses via subtraction-safe RVA offset from dynamic image_base",
    "TOOL-3": "Resolve UScriptStruct::ICppStructOps vtable traits via headless Rizin CFG analysis",
    "TOOL-4": "Cross-check USMAP property reflection indices against generated native schema_types.h",
    "TOOL-5": "Drain Tier 1 CModule 16 MB lock-free SPSC ring buffer off the 60 Hz client thread",
    "TOOL-6": "Read post-call bit cursor from FArchive+0x48 rather than bool return value",
    "TOOL-7": "Exclude OS and driver modules in Tier 2 Stalker before invoking probeStructNetSerialize",
    "TOOL-8": "Validate PCAP (0xa1b2c3d4) or PCAPNG (0x0A0D0D0A) header framing before packet slicing",
    "TOOL-9": "Ingest packets, bunches, rep_ops, rpc_calls, and bit_cursor_deltas into sniff_analysis.db",
    "TOOL-10": "Enforce leftover_bits == 0 and is_error == false across all FRepLayout receive ops",
    "TOOL-11": "Decode ObjectId == 1 inline export path with flags & 1 (GUID + path) and flags & 4 (checksum)",
    "TOOL-12": "Hydrate MovementBase inline component path via read_object_value(r, allow_inline_path=True)",
    "TOOL-13": "Frame UE5.6 TOptional<T> properties with 1-bit presence flag before value payload",
    "TOOL-14": "Frame UE5.6 TSet<T> properties via sparse-array delta serialization contract",
    "TOOL-15": "Reconcile Static S x Runtime R x Server I tables to prioritize un-decoded NetSerializeStruct gaps",
    "TOOL-16": "Author Clean-Room Chinese-Wall markdown spec with 3+ synthetic hex vectors before C++ codec",
    "TOOL-17": "Scrub raw PE32+ image-base hex literals and decompiler symbol prefixes prior to commit",
    "TOOL-18": "Verify round-trip bit-oracle parity encode(decode(frame)) == frame across golden corpus",
    "TOOL-19": "Confirm zero dropped ring-buffer records and <10 ms synthetic bunch injection latency",
}


@dataclass(frozen=True, slots=True)
class InlineExportPathRecord:
    object_id: int
    export_flags: int
    outer_guid: bytes
    path_name: str
    checksum: int | None = None

def decode_inline_export_path(buf: bytes) -> InlineExportPathRecord:
    if len(buf) < 5:
        raise ValueError("inline export header shorter than 5 bytes")
    obj_id, flags = struct.unpack_from("<IB", buf, 0)
    if obj_id != 1:
        raise ValueError("InlineExportPathRecord requires ObjectId == 1")
    cursor, outer_guid, path_name, checksum = 5, b"", "", None
    if flags & 1:
        if len(buf) - cursor < 18:
            raise ValueError("truncated inline export outer GUID or path length")
        outer_guid = bytes(buf[cursor : cursor + 16])
        path_len = struct.unpack_from("<H", buf, cursor + 16)[0]
        cursor += 18
        if path_len == 0 or len(buf) - cursor < path_len:
            raise ValueError("invalid inline export path string bounds")
        path_name = buf[cursor : cursor + path_len].decode("utf-8")
        cursor += path_len
    if flags & 4:
        if len(buf) - cursor < 4:
            raise ValueError("truncated inline export 32-bit checksum")
        checksum = struct.unpack_from("<I", buf, cursor)[0]
    return InlineExportPathRecord(obj_id, flags, outer_guid, path_name, checksum)


@dataclass(frozen=True, slots=True)
class BunchDecodeAudit:
    bunch_id: int
    total_bits: int
    pos_bits: int
    is_error: bool = False

    @property
    def leftover_bits(self) -> int:
        return self.total_bits - self.pos_bits

    def validate(self) -> None:
        if self.is_error or self.pos_bits < 0 or self.pos_bits > self.total_bits or self.leftover_bits != 0:
            raise ValueError(f"bunch {self.bunch_id} failed bit audit: leftover={self.leftover_bits}")

def test_capture_pipeline_inline_export_path() -> None:
    guid = bytes(range(16))
    raw = struct.pack("<IB", 1, 0x05) + guid + struct.pack("<H", 13) + b"CharacterMesh" + struct.pack("<I", 0xCAFEBABE)
    rec = decode_inline_export_path(raw)
    assert rec.object_id == 1 and rec.outer_guid == guid and rec.path_name == "CharacterMesh" and rec.checksum == 0xCAFEBABE


def test_capture_pipeline_bunch_audit_and_diagnostics() -> None:
    assert len(TOOL_DIAGNOSTICS) == 19 and all(f"TOOL-{i}" in TOOL_DIAGNOSTICS for i in range(1, 20))
    BunchDecodeAudit(bunch_id=7, total_bits=128, pos_bits=128).validate()
