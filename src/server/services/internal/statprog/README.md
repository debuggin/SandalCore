# `statprog` — Stat & Ability Expression Compiler, Binary Format & Go VM

`internal/statprog` compiles the design database's character and NPC stat formula graph (`StatTypeDef`, `StatInitializerList`, `EvalE`, `EvalFormula`) and ability/loot/node/artisan expressions into a deterministic, statically verified binary bytecode bundle (`AOSP` format version 3). `cmd/contentc` embeds the compiled bundle into `dist/content/content.db` (`stat_program` table), where both the Go interpreter (`internal/statprog`) and the C++23 interpreter ([`src/libs/aocstats`](../../../../libs/aocstats/README.md)) execute it with bit-for-bit identical IEEE-754 results.

```
content.db --LoadContentDB--> Graph --Compile / CompileExprs--> Bundle --Encode--> AOSP bytes
                                                                                       |
                                   +--- Go (statprog):   Decode (verify) --> Run / Evaluator.Eval
                                   +--- C++ (aocstats):  parse_bundle    --> run_stats / ExprEvaluator
```

---

## 1. Stat Evaluation Model

For a character or NPC of a given class, level, equipment loadout, active effects, and skill ranks, `Bundle.Order` defines a topological evaluation sequence in which every stat's initial value and final value are computed only after all prerequisite stats have settled:

1. **Initial Value**:
   - **Player Classes (`ECharacterClass` `1..8`)**: Evaluates entries from `CombatStats`, `Base_Artisan_Stats`, `Player_Base`, and `Player_Base_<Class>` in order.
   - **NPC Profile Classes (`>= 32`, `NpcProfiles`)**: Evaluates `CombatStats`, `NPC_Base`, and `NPC_Elite_Scalar` (`"NPC"`) or the Guard scalar (`"NPC_Guard"`), parameterized by `StatDistribution` weights and `EliteLevelRanks`.
   - **Running Total Folding (`GetStatInitRunningTotal()`)**: A stat starts at `0.0` (or `1.0` when `bMultiplyStatMods` is set). When an initializer expression calls `GetStatInitRunningTotal()`, the running total includes the stat's `Base` formula folded in (`initial + Base` for additive stats, or `initial * Base` for multiplicative stats), and the initializer's result updates the stored initial value to `result - Base` (or `result / Base`) so the subsequent value pass adds `Base` once. The compiler schedules any initializer that calls `GetStatInitRunningTotal()` after all stats read by its `Base` formula, and the bytecode verifier rejects any bundle that violates this dependency order.
2. **Base, Equipment, Effects & Clamping**:
   - Computes `initial + Base` (`initial * Base` for multiplicative stats), adds summed equipment contributions (`ItemStatBlock` values interpolated along the item's `QualityCurve` plus active `SetBonus` tiers), applies active `Effect` stat modifiers (`Inputs.Effects`), and clamps the result to `[ValueMin, ValueMax]`.
   - Stats flagged with `Reset_SetMax` (`Health`, `Mana`, `Stamina`, `Ranger_Focus`) spawn at their computed maximum; stats flagged with `Reset_SetMin` spawn at their minimum.
3. **Regeneration**:
   - Evaluates each stat's `RegenRate` program after all final stat values have settled (allowing `Fighter_Momentum` decay or Cleric `Divine Power` regeneration to read settled primary and secondary stats).

### Arithmetic Modes & Float32 Boundary Precision

`Bundle.Mode` (encoded in the header flags) controls rounding behavior:

| Mode | Internal Stack Arithmetic | Captured Level-1 Bard (`442` stats) | Notes |
|:---|:---|:---:|:---|
| **`ModeFloat32` (Default)** | `float64` operations; every formula return value, initial value, and final stat value is rounded to `float32` | **`442 / 442`** | Matches retail client precision (`PhysicalSpeed = -0.3 + float32(1.0 + 48/(48+8000))`) |
| **`ModeDouble`** | `float64` throughout | `440 / 442` | `PhysicalSpeed` and `HealingCriticalPower` differ by 1 `float32` ULP |
| **`ModeSingle`** | All operations, constants, and curve lookups in `float32` | `440 / 442` | `HealthRegeneration` and `ManaRegeneration` differ by 1 `float32` ULP |

---

## 2. `AOSP` Binary Format (Version 3) & Verifier Invariants

All multi-byte fields are little-endian. Both `statprog.Decode` (Go) and `aoc::world::stats::parse_bundle` (C++) validate the entire bundle before execution.

```
Header (16 B)   "AOSP" | u16 version = 3 | u16 flags (1 ModeSingle, 2 ModeDouble) | u32 body_len | u32 crc32 (IEEE)
Body            u32 n, f64 consts[n]                      Finite IEEE-754 float64 constants
                u32 n, (u8 len, bytes)[n]                 Strings (stat/class names, gameplay tags, slots, professions)
                u32 n, (u64 type_id, i64 guid)[n]         Design-data references (effects, skills, classes, stats)
                u32 n, curves[n]                          u32 keys (>= 1), (f64 time, f64 value, u8 interp)[keys]
                                                          interp: 0 linear, 1 constant, 2 cubic (fault if evaluated)
                u32 n, programs[n]                        u16 locals, u32 code_len, u8 code[code_len]
                u32 n, stats[n]                           i64 guid, u32 name, u8 replication, u8 flags,
                                                          i32 base, min, max, regen (program index or -1)
                u32 n, (u32 stat, u32 program)[n]         Common initializer list steps
                u32 n, u32[n]                             Common initializer stat indices
                u32 n, classes[n]                         u8 class_enum, u32 name, steps, members
                u32 n, u32[n]                             Topological order: (stat_idx << 1) | kind (0 initial, 1 value)
                u32 n, u32[n]                             Expression program indices (strictly increasing)
```

### Bytecode Opcodes

| Opcode Range | Mnemonics & Operands | Semantics |
|:---|:---|:---|
| **`01..07`** | `01 CONST u16`, `02 LOAD u8`, `03 STORE u8`, `04 STAT u16`, `05 TOTAL`, `06 LEVEL`, `07 POP` | Push constant, local, settled stat, running total, or level; store local; pop stack |
| **`10..15`** | `10 ADD`, `11 SUB`, `12 MUL`, `13 DIV`, `14 NEG`, `15 NOT` | Arithmetic and boolean negation (`DIV` by zero pushes `0.0` and increments `Faults`) |
| **`20..25`** | `20 LT`, `21 LE`, `22 GT`, `23 GE`, `24 EQ`, `25 NE` | Comparisons pushing `1.0` or `0.0` |
| **`30..32`** | `30 JMP u16`, `31 JZ u16`, `32 JNZ u16` | Forward-only absolute jump targets (`&&`, `\|\|`, `if`) |
| **`40..48`** | `40 ROUND`, `41 FLOOR`, `42 CEIL`, `43 MIN`, `44 MAX`, `45 SEL`, `46 CURVE u16`, `47 LERP`, `48 ABS` | Math intrinsics, curve evaluation, ternary `SEL` (`c != 0 ? a : b`), `LERP` (`a + t * (b - a)`), `ABS` |
| **`50..59`** | `50 EFFECT u16`, `51 EFFECT_TAG u16`, `52 SKILL u16 u16`, `53 COMBAT`, `54 MOVING`, `55 UNKILLABLE`, `56 EQUIP_TAG u16 u16`, `57 PROFESSION u16`, `58 CONSUMED u16`, `59 NPC_RANK` | Character and NPC state queries (`aocexpr.State`) |
| **`5A..60`** | `5A PROGRESS u16`, `5B EFFECT_INST u16`, `5C RAND`, `5D HIT_TAG u16`, `5E HIT_FLAG u16`, `5F XP_DEBT`, `60 HIT_TARGETS` | Ability/hit expression host operations (progress ratio, instigator effect count, `SplitMix64` draw, hit tags/flags/targets, XP debt) |
| **`61..64`** | `61 OpHost0 u16 u16`, `62 OpHost1 u16 u16`, `63 OpHost2 u16 u16`, `64 OpActor u8` | Generic host function dispatch (`u16 fn_id + u16 str_idx`) and actor role scope switch (`caster`, `target`, `instigator`, `owner`) |
| **`FF`** | `FF RET` | Returns the single value on top of the stack |

### Stat Bit Flags

| Bit | Flag | Meaning |
|:---:|:---|:---|
| `1` | `StatMulMods` | Multiplicative stat (`bMultiplyStatMods`) |
| `2` | `StatClampFinal` | Final clamp flag recorded from `StatTypeDef` |
| `4` | `StatSpawnMax` | Spawns at computed maximum (`Reset_SetMax`: `Health`, `Mana`, `Stamina`, class resources) |
| `8` | `StatSpawnMin` | Spawns at computed minimum (`Reset_SetMin`) |
| `16` | `StatRepBreakdown` | Replicates `Base` and `EquipmentValue` alongside total (`Artisan_Gathering_Fishing_Sport_*`) |
| `32` | `StatFloatArray` | Replicates via `StatRepFloatArrayEveryoneProxy` (`Type_FloatArray`: `TempHealth`) |
| `64` | `StatMulCompl` | Multiplicative complement effect modifier (`bMultiplyCompliment`: `value *= (1 - mod)`) |
| `128` | `StatResource` | Held class resource with `min`, `max`, and `regen` exported in `StatOut` |

### Static Verifier Guarantees & Resource Bounds

- Every opcode and operand index is bounds-checked against its table; all jump targets are strictly forward and land on instruction boundaries.
- Every control-flow path to an instruction arrives with an identical stack depth within `0..64`, and every program ends in a single `RET` with stack depth `1` (guaranteeing termination in at most one instruction per code byte).
- Stat-graph programs (initializers, `Base`, `ValueMin`, `ValueMax`, `RegenRate`) may only read stats settled earlier in `Bundle.Order` and are prohibited from executing host opcodes (`5A..64`).
- Hard ceilings: bundle size `<= 4 MiB`; `<= 65,535` constants, strings, references, programs, and steps; `<= 4,096` curves, curve keys, and stats; `<= 16` classes; `<= 8,192` code bytes per program; `<= 64` locals; `<= 255` bytes per string; `MaxRunInstructions = 4,194,304` per run.

---

## 3. Expression Compiler & Host Function Domains

`CompileExprs(graph, opts, exprs)` compiles ability, effect, hit, loot, city-node, and gathering expressions into the same `Bundle` and records their program indices in `Bundle.Exprs`. If an expression uses an unsupported construct, `CompileExprs` rolls back the partial emission (`snapshot`/`restore`) and records the diagnostic on `ExprResult` without altering the bundle.

### Compiler Rules

- **Static Typing & Float Division**: Stack values are numbers or booleans; object roles (`GetOwner()`, `GetTarget()`, `GetInstigator()`, `GetSource()`, `GetSelf()`), record references (`$#<type>:<guid>$`, `$Stat:MaxStamina$`, `$F:Name$`), strings, and enum literals are resolved at compile time. The `/` operator requires an operand proven to be floating-point (float literal, `Float`, `SelectFloat`, `EvalCurve`, `GetStat`, or a float host function), while `Env.IntDivide` is available for integer vendor pricing expressions.
- **Term Resolution (`GetTerm`)**: Resolves in innermost-to-outermost lexical scope (`ValueInputTerms` of a `StatMod`,including sibling `StatMod` terms -> caller formula terms -> `TermType.DefaultValue`, where `""` defaults to `0.0`), bounded by `Graph.Terms` min/max limits.
- **Case-Insensitive Gameplay Tags (`TagMatches`)**: Matches `FName` gameplay tags and hierarchical prefixes case-insensitively (`"pet.summon"` matches `Pet.Summon`).
- **Seeded PRNG (`SplitMix`)**: `RandDouble()` and `RandDoubleRange()` draw from `SplitMix64` in deterministic short-circuit evaluation order.

### Host Function Domains (`OpHost0..OpHost2`)

| Host IDs | Domain | Functions |
|:---|:---|:---|
| **`1..11`, `14`** | Ability Runtime | `RemainingCharges` (`1`), `ApplicableTarget` (`2`), `MovementKey` (`3`), `StatDelta` (`4`), `HitStatMod` (`5`), `HitStatEffective` (`6`), `IsNeutral` (`7`), `IsEnemy` (`8`), `AngleBetween` (`9`), `Now` (`10`), `ProgressDuration` (`11`), `RandIntRange` (`14`) |
| **`20..29`** | Combat & `DamageProfile` (`damage.go`) | `CalculateDamage`, `CalculateDamageMultiplier`, `CalculateDamageAdditiveMultiplier`, `CalculateDamageMitigation`, `CalculateDamageCritical`, `CalculateDamagePenetration`, `CalculateDamageDampening`, actor distance, identity, hit source type, and alignment relations |
| **`30..31`, `160..161`** | Kill Rewards & Loot Tables | `GetPlayerLootMaxLevel` (`30`), `GetPlayerLootAvgLuck` (`31`), `DoesNPCContainTag` (`160`), `GetPlayerLevel` (`161`) |
| **`170..177`** | City Nodes (`nodes.go`, `NodeWorld`) | `GetNodeBiome` (`170`), `GetCurrentNode` (`171`), `GetNodeRecord` (`172`), `Node_GetNodeType` (`173`), `Node_GetLevel` (`174`), `Node_InLevelRangeInclusive` (`175`), `Node_IsPlayerCitizen` (`176`), `Node_IsPlayerCitizenOfNode` / `Node_IsPlayerMayor` (`177`). Nodes are represented by their `0..80` sorted `CityNodeRecordBase` rank (`-1` for none) and enums by their `.usmap` ordinals (`aocexpr/enums.go`) |
| **`180..184`** | Gathering & Artisan (`GatherContext`) | `GetArtisanshipModifiedStat` (`180`), `GetRolledItemQuality` (`181`), `GetItemRarity` (`182`), `GetSourceCertificationTier` (`183`), `GetGatheringInteractionBaseDuration` (`184`) |

---

## 4. CLI Tools & Verification

```powershell
cd src/server/services

# Compile content.db stat graph into a standalone bundle (--mode float32|double|single)
go run ./internal/statprog/cmd/statprog compile --db ..\..\..\dist\content\content.db --out stats.bin

# Disassemble and inspect a compiled bundle
go run ./internal/statprog/cmd/statprog dump --in stats.bin

# Regenerate or verify the cross-language Go/C++ test corpus
go run ./internal/statprog/cmd/statprog corpus --out ..\..\..\build\gen\fixtures\corpus.txt
go run ./internal/statprog/cmd/statprog corpus --out ..\..\..\build\gen\fixtures\corpus.txt --check
```

Run the full Go test suite via:

```powershell
pwsh -File sandal.ps1 test
```

Key verification properties tested by `go test ./internal/statprog/...`:

- **Captured Level-1 Bard Parity**: `442/442` stats match the retail capture bit-for-bit in `ModeFloat32`, including replication audience masks (`335` `Owner`, `96` `OwnerProxy`, `11` `EveryoneProxy`) and base/equipment breakdown values on `Artisan_Gathering_Fishing_Sport_*`.
- **Reference Engine Equivalence**: Compiled bytecode execution matches `RefEngine` (AST interpretation over raw formula text) bit-for-bit across all 8 player classes at levels 1, 13, 20, and 50 (bare and with starting kits) and across all 2,400 `class_stat_base` attribute values.
- **Cross-Language Corpus (`TestCorpusUpToDate`, `TestCorpusReplay`)**: Verifies 5 bundles, 38 character profiles, 14 `fxcase` ability/effect suites (3,640 expression evaluations and 12 retail benchmark numbers), `dprofile`/`damage` NPC combat vectors, `nodehost` and `gatherhost` blocks, 98 hostile malformed bundles, and 28,000 differential fuzz mutations shared with `libaocstats`.
