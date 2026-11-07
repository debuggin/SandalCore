# Wire Protocol Specifications

**Related Guides**: [Root Overview](../../README.md) | [Wire Protocol Library (`libaocnet`)](../../src/libs/aocnet/README.md) | [World Server](../../src/server/world/README.md) | [Contributing Guide](../CONTRIBUTING.md)

This directory contains the clean-room wire protocol specifications for Unreal Engine 5.6 UDP bit-stream framing, property replication, RPC serialization, the `127.0.0.1:19021` launcher tether protocol, and the `:50061` ICS TLS 1.3 lobby protocol implemented in [`src/libs/aocnet/`](../../src/libs/aocnet/README.md).

Every specification records normalized module-relative RVAs, bit-level field layouts, overflow-safe bounds checks, and synthetic hex test vectors, with zero proprietary source code or decompiler output.

---

## Three-Way Wire Verification Pipeline

The reverse-engineering workbench (`scripts/re_workbench.ps1`) verifies every wire codec across three sources before it enters `src/libs/aocnet/`:

1. **Static Schema Extraction (`static_rizin_usmap`)**: Extracts struct serialization flags (`WithNetSerializer`, `WithNetSharedSerialization`) and property handles from `.usmap` mappings pinned by executable `SHA-256`, `PDB GUID`, `EngineNetVer`, and `GameNetVer`.
2. **Runtime Bit-Cursor Tracing (`frida_session`)**: Records pre- and post-call `FArchive` bit-cursor positions through a 65,536-slot lock-free ring buffer in `src/tools/re-workbench/cmodule/ring_hook.c` and `src/tools/re-workbench/agent/index.ts`.
3. **Differential Bit-Oracle Reconciliation (`bit_oracle` and `reconcile_3way`)**: Replays captured packets through the wire codecs in `src/libs/aocnet/` and verifies zero read errors (`is_error == false`) and zero trailing bits (`leftover_bits == 0`).

---

## Core Struct Codecs and Guards

| ID | Struct or Guard | Implementation File | Wire Purpose |
|:---|:---|:---|:---|
| `P0-01` | `FEssentialCitizenshipWindowData` | `AAoCPlayerController.h` / `codecs_containers.cpp` | Node citizenship window bit-stream |
| `P0-02` | `FNodeInventoryData` | `AAoCPlayerController.h` / `codecs_containers.cpp` | Settlement warehouse and node storage sync |
| `P0-03` | `FClientBuyOrderData` | `AAoCPlayerController.h` / `codecs_containers.cpp` | Construction and material buy-order entries |
| `P0-04` | `FNetLevelVisibilityTransactionId` | `AAoCPlayerController.h` / `codecs_containers.cpp` | Packed `uint32` sub-level visibility transaction ID |
| `P0-05` | `FCommissionBoardList` | `AAoCPlayerController.h` / `codecs_containers.cpp` | Settlement commission board task array |
| `P0-06` | `FUpdateLevelVisibilityLevelInfo` | `AAoCPlayerController.h` / `codecs_containers.cpp` | Streaming sub-level visibility handshake |
| `P0-07` | `FRootMotionSourceGroup` | `AAutonomousPlayerCharacter.h` / `codecs_containers.cpp` | Ability root-motion source group state |
| `P0-08` | `FSharedRepMaster` | `AAutonomousPlayerCharacter.h` / `codecs_containers.cpp` | Shared movement replication state block |
| `P0-09` | `FClientSpawnDeployableData` | `AAoCPlayerController.h` / `codecs_containers.cpp` | Camp, siege engine, and mobile station spawn payload |
| `P0-10` | `FMapMarkerOverrideWrapper` | `AAoCPlayerController.h` / `codecs_containers.cpp` | Dynamic event and caravan map marker override |
| `P0-11` | `FPlayerCommodityCounts` | `AAoCPlayerController.h` / `codecs_containers.cpp` | Glint, scrip, and regional commodity counters |
| `P0-12` | `FCharacterMoveResponsePackedBits` | `AAutonomousPlayerCharacter.h` / `move_packed.cpp` | Server-to-client movement correction bit-stream |
| `Guard` | `PropType::Unknown` (`TOptional` / `TSet`) | `src/libs/aocnet/protocol/repl/rep_block.h` | Presence-bit and sparse-set bounds guard |

---

## Running Protocol Verification

Run the protocol workbench and clean-room safety checks from PowerShell 7:

```powershell
pwsh -NoProfile -File scripts/re_workbench.ps1
pwsh -NoProfile -File scripts/checks/public_check.ps1
```
