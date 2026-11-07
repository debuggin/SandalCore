# NPC Abilities Extractor

**Related Guides**: [Root Overview](../../../README.md) | [Game-Data Extractor](../aoc-extractor/README.md) | [World Server](../../server/world/README.md)

`src/tools/npcabilities` is an extraction utility that inspects extracted Ashes of Creation creature behavior trees, ability blueprints, and animation montages to produce `dist/content/npc_abilities.tsv`.

---

## What `npc_abilities.tsv` Provides

The `world` server loads `npc_abilities.tsv` at startup so hostile creatures and guards use their authentic attack timings, ranges, and animations:

- **Creature Family Mapping**: Maps creature archetypes (`Beast`, `Humanoid`, `Goblin`, `Bandit`, `Undead`, `Elemental`, `Construct`) to their authored ability IDs (`429` distinct creature attacks).
- **Attack Timing and Range**: Records cast wind-up time, hit frame, cooldown duration, minimum and maximum attack range in centimeters, and projectile speed.
- **Status Effect Payloads**: Links creature attacks to applied status effects (`Bleeding`, `Burning`, `Poison`, `Slow`, `Shaken`).

---

## Running the Tool

`npcabilities` runs automatically as part of the extraction pipeline:

```powershell
.\sandal.ps1 extract
```
