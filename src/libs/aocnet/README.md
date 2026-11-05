# `libaocnet` — UE5 / IntrepidNet Wire Protocol & Replication Library

`libaocnet` (`aocnet`) is a standalone C++23 static library implementing the Unreal Engine 5.6 / IntrepidNet UDP wire protocol, bit-stream codecs, generated `RepLayout` class schemas, and stateful actor/subobject replication engine used by `world` (`aoc-world`).

---

## 1. Directory Layout & Physical Boundary

| Directory / Header | Responsibility |
|:---|:---|
| `protocol/conn/` | 14-bit sequence number arithmetic (`sequence.h`), UE5 `FNetPacketNotify` ack bitvectors (`packet_notify.h`), 4-way `StatelessConnect` handshake with the 96-byte Intrepid extension block and cookie-derived initial sequences (`handshake.h`), bidirectional data-packet and `Ch 0` control-bunch framing (`packet_framing.h`), and `NMT_*` control messages (`nmt.h`). |
| `protocol/wire/` | Bit-level UE5 primitives (`ue5_primitives.h`), packet header & `PacketInfo` (`packet_info.h`), bunch headers and `walk_bunches` (`packet_parser.h`), partial-bunch reassembly (`kMaxSingleBunchBits = 7621`, `kMaxPartialBunchBits = 7616`), and low-level RPC payload parsers (`actor_rpc_parser.h`, `level_visibility.h`, `server_move_parser.h`). |
| `protocol/emit/` | Outbound bunch writer, `ActorBuilder`, package-map export framing, and `RepLayout` field emitters (`replayout/`). |
| `protocol/schema/` | Base schema registry and legacy hand-derived class schemas (`actor_rep_handles.h`, `*_schema.cpp`). |
| `protocol/schema/generated/` | Client net-class schemas (`RepLayout` commands, net-field indices, and RPC parameter layouts) generated directly from `AOCClient-Win64-Shipping.exe` and verified via `MANIFEST.sha256`. |
| `protocol/repl/` | Stateful replication engine: typed `Value`s, `RepState`, `NetSerialize` struct codecs, per-connection `RepShadow` delta blocks, `FFastArraySerializer` deltas, typed RPC builders/parsers, package-map `ExportChain`/`ExportAcks`, `SendLedger` NAK recovery, and `open_actor` / `update_actor` / `call_rpc` emitters. |
| `protocol/server_guid_allocator.h` | Deterministic `FIntrepidNetGUID` allocation scoped under a world server's `ServerId`. |
| `protocol/net_guid_allocator.h` | Per-player `NetGUID` allocator utility header. |

### Header Boundary Rule

Translation units in `src/libs/aocnet/` may include only the C++ standard library, `spdlog`, and headers within `src/libs/aocnet/` using the `protocol/` prefix (for example, `#include "protocol/wire/packet_parser.h"`). The `aocnet` CMake target exports `NOMINMAX` on Windows so `std::min` / `std::max` in `ue5_primitives.h` never collide with Windows macros.

---

## 2. Generated Class Schemas (`protocol/schema/generated/`)

The schema generator in [`src/tools/schemagen/`](../../tools/schemagen/) reads `AOCClient-Win64-Shipping.exe` and cooked packages read-only to emit deterministic C++ tables for the client's replicated classes and pawn/actor components:

- **Native & Blueprint Actor Classes**: 49 core class schemas (45 native and 4 Blueprint classes: `PlayerPawn_C`, `AoCPlayerControllerBP_C`, `AoCGameStateBP_C`, `AoCPlayerStateBP_C`), including `ANPCCharacter`, `AGatherableActor`, `AAoCAbilityLingeringEffect`, `APlayerLootPile`, and `ANodeLayoutReplicator`.
- **Component Schemas**: `ULevelingComponent` (`XPTracker`), `UMountComponent` (`GroundMountComponent`, `FlyingMountComponent`, `AquaticMountComponent`, and the `MountComponent_C` alias), `ULootComponent`, `UNPCInfo` (`BaseCharacterInfo`), `UNPCCombatInfo`, `UNPCSecondaryInfo` (`InteractInfo`) and its parent `UCharacterSecondaryInfo`, `UNPCAppearanceComponent`, `UInteractableComponent`, and `UAoCNodePlotComponent`.

Each generated header conforms to `schema_types.h` and defines three flattened tables ordered from root superclass to leaf class:

1. **`RepLayout` Commands (`commands[handle - 1]`)**: 1-based wire handle, dotted property path (`AttachmentReplication.AttachParent`), `PropType`, wire bit width, and `NetSerialize` struct type name.
2. **Net Fields**: Property and RPC field indices, `NumFields` (the `SerializeInt` upper bound for field headers), and `UFunction` flags.
3. **RPC Parameters**: Per-RPC parameter lists flattened into `RepLayout` commands matching `FRepLayout::InitFromFunction`.

`index.h` resolves schemas by native or Blueprint class name (`find_class_schema("AoCPlayerControllerBP_C")`) or by component export name (`find_component_schema("BaseCharacterInfo")`).

### Schema Verification & Blueprint Invariants

```powershell
# Verify that all generated headers and index.cpp match MANIFEST.sha256 (LF-normalized)
pwsh -File scripts/generators.ps1 -Only schemagen -Strict
```

- **Blueprint Class Layout**: The four Blueprint actor classes add zero `CPF_Net` replicated properties beyond their native C++ parent classes; `PlayerPawn_C` (4 functions) and `AoCPlayerControllerBP_C` (6 functions) extend only the net-field table with `FUNC_Net` Blueprint RPC events sorted by name (`BLUEPRINT_NET_FUNCTIONS`).
- **Unsupported Container Guard (`PropType::Unknown`)**: Four properties using `TOptional` or `TSet` (`AAoCGameStateBase::Quests.GeneralQuestMarker`, `AAoCPlayerController::QuestInstance.QuestObjectiveMarker.MapMarkerOverride`, and `AAoCPlayerController::SpawnQueryResults.ChildSocketGuids` / `.UsedChildSocketGuids`) are emitted as `PropType::Unknown` and rejected by `RepState::set` at construction time.

---

## 3. Replication Core (`protocol/repl/`)

`protocol/repl/` provides bit-exact UE5 replication state management, `NetSerialize` codecs, and typed RPC framing verified against retail packet captures.

| Header | Contents |
|:---|:---|
| `value.h` | `Value`: Tagged union of wire-replicated values (`bool`, integers, floats, `FString`, `FName`, `FText`, object references, soft object references, typed `NetSerialize` structs, and dynamic arrays). Quantized vectors store wire integers directly for lossless round-tripping; floats compare by raw IEEE-754 bit pattern. |
| `rep_state.h` | `RepState`: Per-object property map indexed by `RepLayout` handle. `set(schema, "path", value)` validates property path, type, bit width, string NUL-safety, and struct codec availability up front. |
| `codecs.h` | Readers/writers for `FString`, `FName`, `FText`, object/soft-object refs, `FVector`/`FVector2D`/`FRotator`, `FVector_NetQuantize*` (`QuantVec`), `FRepMovement`, `FUniqueNetIdRepl`, gameplay tags, `FStatInt32InstRep`, `FCharacterLoadTracker`, `FQuat`, `FGuidToBoolWrapper`, `FAlignmentContainer`, `FAppearanceId`, `FCharacterCustomizationSaveData`, `FPlayerMovementInput` (4-bit movement direction), and `CodecOptions` (rotation width, quantization levels, extended stat records, customization presets, and `inline_objects`). |
| `rep_block.h` | `write_rep_block` / `read_rep_block` (`bDoChecksum`, ascending handle loop, terminator `0`, dynamic array count + element handles), `RepShadow` per-connection handle tracking, `ParentMask` role filtering, and `SerializeInt(field, NumFields + 1)` field headers. |
| `fast_array.h` | `FFastArraySerializer` delta engine: `FastArrayState` (`ReplicationID`, `ReplicationKey`, array key), `FastArrayShadow`, `write_fast_array_delta`, and `write_fast_array_fields`. |
| `rpc.h` | Bidirectional RPC serialization via `RpcSchema` (`FRepLayout::SendPropertiesForRPC`) and `read_client_bunch` for client-to-server bunches (safely skipping unrecognized or in-place subobject blocks via their bit-length headers and recording `ClientBunch::in_place` / `skipped_fields`). |
| `subobject.h` | Package-map exports (`ExportLink`, `ExportChain`, `pack_exports` into `<= 7621`-bit bunches, `ExportAcks`) and content blocks (`write_content_block`, `read_content_blocks`, `InPlaceName`). |
| `move_packed.h` | `read_move_packed` / `write_move_packed` for `ServerMovePacked` (`FCharacterServerMovePackedBits`: new, pending, and old moves with `TimeStamp`, acceleration, location, control rotation, flags, movement base/mode, and AoC input direction). |
| `ability_rpc.h` | Typed parsers/builders for `ServerTriggerAbility` (`TriggerAbility`, including `inline_targets` for unexported NPC `CollisionCylinder` components), `ServerNotifyHeldAbilityCancelled`, `read_trigger_prefix`, and `ClientEndAbility(ClientInstanceId)`. |
| `item_codec.h` | `NetSerialize` codecs for `FItemInstance` (1,096 bits populated, 1 bit empty; record ID, packed ints, gems, durability, GUID, timestamps, duplicated fields, persisted stats, cooldowns, custom data) and `FItemStorageEvent` (including optional `FCalloutQueueInfo` harvest acquisition popups), with strict element-count bounds (`<= 64` gems/cooldowns/custom/containers, `<= 31` stats, `<= 4096` slots). |
| `storage_rpc.h` | Typed storage and loot RPC framing: `StoragePayload` (`ClientReceiveStoragePayload`), `ClientReceiveStorageEvent` / `ServerRequestStorageEvent`, `LootPayload` (`ClientReceiveLootStoragePayload`), `ServerRequestLootStorageEvents`, and `ClientRemoveListenerFromStorage`. |
| `item_rpc.h` | Typed `ItemUse` parsers/builders for `ServerItemUsed` and `ServerActivateItem` (`Storage`, `Slot`, `ExpectedItem`, `RequestedEquipSlot`). |
| `hit_codec.h`, `hit_rpc.h` | `NetSerialize` codec for `FSerializedClientHitConfirmations` (`HitConfirmations` / `HitInstance` / `HitTarget`, up to 40 targets and 10 stat/hit modifiers per hit) and typed RPC helpers for `RequestHitVerification_Server` (`parse_hit_request` with `CodecOptions::inline_objects`) and `ClientHitConfirmationsReliable` / `ClientHitConfirmations` (`client_hit_confirmations` with `extra_exports` for target NPC `CollisionCylinder` chains). |
| `leveling.h`, `skill_rpc.h` | `ULevelingComponent` (`XPTracker`) `LevelingState` across the 5 fast maps (XP/debt, certifications, skill points, skill tree rank lists) and typed RPC helpers for `ServerPurchaseSkillBuild`, `ClientUpdateSkillOnActionbar`, `ClientReceiveActionbarSlotPayload`, `ServerSetActionbarSlotUseData`, and `ServerSetUseRangedWeapon`. |
| `vendor_codec.h`, `vendor_rpc.h` | `NetSerialize` codecs and RPC helpers for `FVendorPurchaseCart`, `FVendorTransactionCart`, `ServerInteractWithInteractableComponent`, `ClientOpenVendor`, and `FNodeTaxDetails` (`ServerGetNodeTaxTable`, `ClientSetNodeTaxTable`). |
| `craft_rpc.h` | Typed RPC builders/parsers for `ClientOpenCraftingStationUI` (`open_station_params`, `parse_open_station`). |
| `channeling_codec.h` | `NetSerialize` codec for `FChannelingData` (handle 26 on `UCharacterSecondaryInfo` / `InteractInfo`) driving client gathering animations and progress bars. |
| `callout_codec.h` | `NetSerialize` codec for `FLocalizedMessage` on `PlayerController` `CalloutQueueReplication` fast arrays. |
| `block_reader.h` | `load_object_state` converting captured `RepLayout` and fast-array content blocks into live `ObjectState` templates. |
| `replicator.h`, `send_ledger.h` | `Actor`, `ActorShadow`, `open_actor`, `update_actor`, `call_rpc` / `call_rpcs`, and `SendLedger` per-channel unreliable NAK recovery. |

### Usage Example

```cpp
repl::Actor pc = make_pc();                      // Schema, RepState values, open info, subobjects
repl::ActorShadow shadow;                        // Per-connection replication shadow
auto open = repl::open_actor(pc, shadow, acks);  // First emission on the actor channel
world::Chain c{.channel = 3, .open = true, .exports = open->exports, .payload = open->payload};

// Subsequent delta update: work on a shadow copy and commit only when send_chain succeeds
repl::ActorShadow next = shadow;
auto e = repl::update_actor(pc, next, acks, &failed);
if (e && connection.send_chain(to_chain(3, false, *e))) {
    shadow = std::move(next);
}
```

### Caller Contracts

1. **Atomic Shadow Commit**: `open_actor` and `update_actor` advance `ActorShadow` in place. Always pass a copy of `ActorShadow` and commit it only after `Connection::send_chain` accepts the resulting `Chain` so congestion rejections never drop property deltas.
2. **Deferred `ExportAcks` Confirmation**: Mark `Emission::exported_by_bunch[i]` in `ExportAcks` only when `Connection::take_ack_events` reports packet delivery for export bunch part `i` (never at enqueue time), matching `UPackageMapClient::ReceivedAck`.
3. **Unreliable NAK Recovery (`SendLedger`)**: For unreliable actor channels (`ActorView`), record each sent update in `SendLedger::sent` and feed `AckEvent` notifications to `SendLedger::on_event`. On an unreliable packet drop, `SendLedger` rolls back unacknowledged handle shadows and sets `FastArrayShadow::resend` (`kUnknownKey`) so the next tick transmits the latest state (`FRepLayout::ReceivedNak`). Callers must suppress unreliable updates while `Connection::reliable_pending()` is true on that channel.
4. **Fast-Array Dirty Marking**: After mutating a `FastArrayState` (`insert`, modify, or erase), call `mark_array_dirty()` (or `set_array_key()`) so the delta emitter increments the array replication key.

---

## 4. Offline Capture Headers & Verification Suite

Committed test fixture headers (`src/tests/cpp/net/capture_*.h`: `capture_blocks.h`, `capture_aoc_codecs.h`, `capture_moves.h`, `capture_abilities.h`, `capture_items.h`, `capture_hits.h`, `capture_leveling.h`, `capture_mount.h`, `capture_npc.h`, `capture_vendor.h`) are generated from packet captures via [`src/tools/schemagen/`](../../tools/schemagen/):

```powershell
# Verify that all committed capture headers and generated schemas are up to date
pwsh -File scripts/generators.ps1 -Strict
```

Run the full C++ protocol and replication test suite via:

```powershell
pwsh -File sandal.ps1 test
```

The test targets in [`src/tests/cpp/`](../../tests/cpp/) verify bit-for-bit round-trip fidelity and fuzz resilience across every protocol layer:

- **Connection & Wire Framing**: Sequence wrap-around, `FNetPacketNotify` ack bitvectors, 4-way `StatelessConnect` handshake and cookie verification, `Ch 0` `NMT_*` control messages, and partial-bunch reassembly.
- **Generated Schemas & Core Replication (`test_generated_schemas`, `test_repl_codecs`, `test_repl_aoc_codecs`, `test_repl_rep_block`, `test_repl_fast_array`, `test_repl_rpc`, `test_repl_actor_open`, `test_repl_send_ledger`)**: Bit-exact decoding and re-encoding of captured actor opens, delta blocks, fast arrays, and 280-run seeded packet-loss soak tests.
- **Subsystem RPC & Struct Codecs (`test_repl_move_packed`, `test_repl_ability_rpc`, `test_repl_item_rpc`, `test_repl_items`, `test_repl_hits`, `test_repl_leveling`, `test_repl_mount`, `test_repl_npc`, `test_repl_vendor`, `test_repl_gather`, `test_repl_loot`)**: Bit-for-bit round-trip checks against thousands of retail movement, ability, inventory, loot, combat hit, skill tree, mount, NPC, vendor, tax table, and gathering blocks, paired with seeded heap-bounded truncation and bit-flip fuzzing.
