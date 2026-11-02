# Clean-Room Wire Protocol Specifications (`docs/reference/wire-specs/`)

> **Diátaxis Quadrant**: Technical Reference

This directory indexes the clean-room bit-stream wire specifications for Unreal Engine 5.6 replication and RPC codecs implemented in `libs/aocnet/protocol/`. Every specification enforces the Chinese-Wall Specification Firewall (`CS-13`, `SEC-06`, `SEC-08`): documents MUST record normalized module-relative RVAs, bit-level field layouts, subtraction-safe bounds predicates, and synthetic hex test vectors, and MUST NOT contain proprietary source code, raw PE32+ virtual addresses, or decompiler auto-symbols.

## 1. Clean-Room 3-Way Wire Protocol Verification Pipeline

The reverse-engineering workbench (`tools/re-workbench/`) reconciles three independent sources before a codec enters `libs/aocnet/`:

1. **Static Schema (`S` — Rizin CFG + USMAP)**: Extracts `UScriptStruct::ICppStructOps` trait flags (`WithNetSerializer`, `WithNetSharedSerialization`) and property handles pinned by `(ExecSHA256, PdbGUID, EngineNetVer, GameNetVer)` in `re_workbench/static_rizin_usmap.py` and `re_workbench/manifest_verifier.py`.
2. **Runtime Instrumentation (`R` — Frida `CModule` Ring Hook)**: Captures pre- and post-call `FArchive` bit-cursor positions (`BitTraceSlot`, `< 150 ns` hot-path budget) via a 65,536-slot lock-free SPSC ring buffer in `cmodule/ring_hook.c` and `re_workbench/frida_session.py`.
3. **Differential Bit-Oracle & 3-Way Reconciler (`I` — `0`-Residue Verification)**: Cross-joins static schemas, runtime bit deltas, and server codecs in `re_workbench/bit_oracle.py`, `re_workbench/reconcile_3way.py`, `re_workbench/gap_correlator.py`, and `re_workbench/spec_generator.py`, requiring `is_error == False` and `leftover_bits == 0` across all captured bunches.

## 2. P0 `NetSerializeStruct` Wire Codecs & `PropType::Unknown` Guard

| Priority | Struct / Guard Target | Schema & Codec Location | Wire Framing Contract |
| :--- | :--- | :--- | :--- |
| `P0-01` | `FEssentialCitizenshipWindowData` | `AAoCPlayerController.h` / `codecs_containers.cpp` | Node citizenship status modal bit-stream |
| `P0-02` | `FNodeInventoryData` | `AAoCPlayerController.h` / `codecs_containers.cpp` | Warehouse and node storage synchronization |
| `P0-03` | `FClientBuyOrderData` | `AAoCPlayerController.h` / `codecs_containers.cpp` | Construction and material buy-order entries |
| `P0-04` | `FNetLevelVisibilityTransactionId` | `AAoCPlayerController.h` / `codecs_containers.cpp` | Packed `uint32` sub-level visibility transaction ID |
| `P0-05` | `FCommissionBoardList` | `AAoCPlayerController.h` / `codecs_containers.cpp` | Settlement commission board offering array |
| `P0-06` | `FUpdateLevelVisibilityLevelInfo` | `AAoCPlayerController.h` / `codecs_containers.cpp` | Streaming sub-level visibility handshake |
| `P0-07` | `FRootMotionSourceGroup` | `AAutonomousPlayerCharacter.h` / `codecs_containers.cpp` | Ability root-motion source group state |
| `P0-08` | `FSharedRepMaster` | `AAutonomousPlayerCharacter.h` / `codecs_containers.cpp` | Shared movement replication master block |
| `P0-09` | `FClientSpawnDeployableData` | `AAoCPlayerController.h` / `codecs_containers.cpp` | Camp, siege engine, and mobile station spawn data |
| `P0-10` | `FMapMarkerOverrideWrapper` | `AAoCPlayerController.h` / `codecs_containers.cpp` | Dynamic event and caravan map marker override |
| `P0-11` | `FPlayerCommodityCounts` | `AAoCPlayerController.h` / `codecs_containers.cpp` | Glint, scrip, and regional commodity counters |
| `P0-12` | `FCharacterMoveResponsePackedBits` | `AAutonomousPlayerCharacter.h` / `move_packed.cpp` | Server-to-client movement correction bit-stream |
| `Guard` | `PropType::Unknown` (`TOptional` / `TSet`) | `libs/aocnet/protocol/repl/rep_block.h` | Optional presence-bit and sparse-set command guard |

## 3. Reproducible CLI Verification

Run the hermetic workbench and clean-room firewall verification suite from the repository root:

```bash
uv run --project tools/re-workbench pytest
pwsh -NoProfile -File scripts/checks/public_check.ps1
```
