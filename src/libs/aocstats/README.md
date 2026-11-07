# Stat Evaluation Library

**Related Guides**: [Root Overview](../../../README.md) | [Stat Bytecode Engine](../../server/services/internal/statprog/README.md) | [Content Compiler](../../server/services/cmd/contentc/README.md) | [World Server](../../server/world/README.md)

`src/libs/aocstats` (`libaocstats`) is the virtual machine that evaluates compiled `AOSP` stat bytecode programs inside `world`. It works in lockstep with the stat bytecode compiler in [`src/server/services/internal/statprog`](../../server/services/internal/statprog/README.md) so character stats, item ratings, and combat formulas produce identical results across control-plane services and zone workers.

---

## How It Works

1. **Offline Compilation (`contentc`)**: During `content.db` (`v41`) compilation, `contentc` compiles UE5 DesignData expression trees and curve tables into compact `AOSP` bytecode blobs stored in the `stat_programs` and `curves` SQLite tables.
2. **Zero-Allocation Runtime Evaluation (`libaocstats`)**: At runtime, `world` loads those bytecode blobs into `libaocstats` and evaluates primary attributes (`Might`, `Dexterity`, `Intellect`, `Wisdom`, `Constitution`, `Mentality`), derived combat ratings (`PhysicalPower`, `MagicalPower`, `CritChance`, `Mitigation`, `MoveSpeed`), item rarity scaling (`Common` to `Artifact`), and ability damage/healing formulas without dynamic heap allocations on the combat hot path.

---

## Running Tests

Parity tests verify that `libaocstats` and `internal/statprog` return identical outputs across the stat test corpus:

```powershell
.\sandal.ps1 test
```
