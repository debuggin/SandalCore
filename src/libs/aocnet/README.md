# Wire Protocol Library

**Related Guides**: [Root Overview](../../../README.md) | [Wire Protocol Specifications](../../../docs/reference/wire-specs/README.md) | [World Server](../../server/world/README.md)

`src/libs/aocnet` (`libaocnet`) is the clean-room networking and serialization library that implements the Unreal Engine 5.6 UDP `IpNetDriver` wire protocol for Ashes of Creation (`Verra_01`).

---

## Architecture and Layers

`libaocnet` processes UDP game traffic across four layers:

1. **Bit-Stream Reader and Writer (`bitstream/`)**: Reads and writes UE5 bit-level primitives (`BitReader`, `BitWriter`, packed integers, quantized vectors, `FName`, `FText`, `FGuid`, and `FNetworkGUID`) with strict bounds checking before every cursor advance.
2. **Packet and Bunch Framing (`packet/`, `bunch/`)**: Handles the 96-byte `StatelessConnect` handshake, packet sequence/ack headers (`SeqNumber<14>`), reliable and partial bunch reassembly, and channel lifecycle (`Control`, `Actor`, `Voice`).
3. **Property Replication and RPC Codecs (`protocol/`)**: Encodes and decodes replicated actor properties (`RepBlock`, `RepLayout`) and Remote Procedure Calls (`AAoCPlayerController`, `AAutonomousPlayerCharacter`, `ACharacter`, combat, inventory, crafting, quest, and node structs).
4. **Session Transport (`session/`)**: Manages per-client UDP encryption, keepalives, channel table state, and outgoing packet flushing for `world` and `fake::Client`.

---

## Source Layout

| Directory | Purpose |
|:---|:---|
| `bitstream/` | Bit-level readers, writers, and quantized UE5 math types (`FVector_NetQuantize*`, `FRotator`). |
| `packet/` & `bunch/` | UDP packet headers, ack bitvectors, reliable bunch windowing, and partial bunch reassembly. |
| `protocol/` | Actor replication blocks, `NetSerializeStruct` codecs, and generated RPC dispatch tables. |
| `session/` | Client/server UDP session state machines used by `world` and the `fake::Client` test harness. |

---

## Running Protocol Tests

```powershell
# Run all unit and wire-codec replay tests
.\sandal.ps1 test
```
