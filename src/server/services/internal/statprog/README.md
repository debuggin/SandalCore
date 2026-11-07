# Stat Bytecode Engine

**Related Guides**: [Root Overview](../../../../../README.md) | [Stat Evaluation Library (`libaocstats`)](../../../../libs/aocstats/README.md) | [Content Compiler](../../cmd/contentc/README.md) | [Services Overview](../../README.md)

`src/server/services/internal/statprog` is the compiler and virtual machine for SandalCore's `AOSP` stat bytecode format. It compiles Unreal Engine 5.6 DesignData stat expressions and curve tables into verified bytecode stored in `content.db` (`v41`) and executed at runtime by both control-plane services and the `world` server (`libaocstats`).

---

## Responsibilities

- **Expression Compilation**: Parses DesignData formula graphs (`EvalExpression`, `CurveTable`, `AttributeMod`, `SkillRank`, and `ItemRarity` nodes) during `contentc` compilation and emits compact, bounds-checked `AOSP` bytecode instructions.
- **Deterministic Virtual Machine**: Evaluates compiled `AOSP` programs using fixed-stack IEEE-754 arithmetic that matches the `libaocstats` evaluator bit-for-bit.
- **Parity Testing**: Verifies that every compiled stat curve and ability formula in `content.db` produces identical outputs in `statprog` and `libaocstats`.

---

## Running Tests

```powershell
.\sandal.ps1 test
```
