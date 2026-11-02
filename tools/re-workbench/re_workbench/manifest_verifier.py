from __future__ import annotations
from dataclasses import dataclass
import hashlib
import struct


@dataclass(frozen=True, slots=True)
class ManifestPin:
    exec_sha256: str
    pdb_guid: str
    pdb_age: int

@dataclass(frozen=True, slots=True)
class PeRsdsInfo:
    exec_sha256: str
    pdb_guid: str
    pdb_age: int
    is_pe32_plus: bool

def parse_pe_rsds(pe_bytes: bytes) -> PeRsdsInfo:
    if len(pe_bytes) < 0x100 or struct.unpack_from("<H", pe_bytes, 0)[0] != 0x5A4D:
        raise ValueError("TOOL-1: invalid PE32+ header")
    e_lfanew = struct.unpack_from("<I", pe_bytes, 0x3C)[0]
    if e_lfanew < 0x40 or len(pe_bytes) - e_lfanew < 26:
        raise ValueError("TOOL-1: invalid PE32+ header")
    pe_sig = struct.unpack_from("<I", pe_bytes, e_lfanew)[0]
    opt_magic = struct.unpack_from("<H", pe_bytes, e_lfanew + 24)[0]
    if pe_sig != 0x00004550 or opt_magic != 0x020B:
        raise ValueError("TOOL-1: invalid PE32+ header")
    idx = pe_bytes.find(b"RSDS")
    if idx < 0 or len(pe_bytes) - idx < 24:
        raise ValueError("TOOL-1: missing RSDS CodeView record")
    pdb_guid = pe_bytes[idx + 4 : idx + 20].hex().upper()
    pdb_age = struct.unpack_from("<I", pe_bytes, idx + 20)[0]
    return PeRsdsInfo(hashlib.sha256(pe_bytes).hexdigest().lower(), pdb_guid, pdb_age, True)

def verify_pe_rsds_manifest(pe_bytes: bytes, pin: ManifestPin) -> PeRsdsInfo:
    info = parse_pe_rsds(pe_bytes)
    if info.exec_sha256.lower() != pin.exec_sha256.lower() or info.pdb_guid.upper() != pin.pdb_guid.upper() or info.pdb_age != pin.pdb_age:
        raise ValueError("TOOL-1: manifest mismatch")
    return info
