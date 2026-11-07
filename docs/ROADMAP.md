# Project Roadmap

**Related Guides**: [Root Overview](../README.md) | [Patch Notes](PATCH_NOTES.md) | [Known Issues](KNOWN_ISSUES.md) | [Server Workers](SERVER_WORKERS.md)

> [!NOTE]
> This roadmap tracks ongoing development across **SandalCore**. Features and milestones evolve continuously as protocol research and server emulation advance.

**Last updated:** 7 November 2026

---

## Current Progress

**5 of 16 phases are complete** (Phases 1-5), **8 are in progress** (Phases 6-13), and **3 are planned** (Phases 14-16).

- **Completed Phases**: `1 - Network Protocol`, `2 - Login`, `3 - Into the World`, `4 - Stats`, and `5 - Mounts`, all verified against retail captures and automated test suites in `src/tests/cpp/` and `src/server/world/tests/`.
- **Active Phases**: `6 - Progression and Items`, `7 - Combat and Abilities`, `8 - Gathering and Professions`, `9 - Living World`, `10 - City Nodes`, `11 - Quests`, `12 - Economy`, and `13 - Social`.

---

## Completed, In-Progress, and Planned Features

### Completed

- **Connection, Login, and Character Creation**: Full launcher sign-in, `:19021` UDP tether ticket exchange, `:50061` ICS TLS 1.3 character creation, entering the world, and saving character state across logins.
- **Multi-Worker Spatial Mesh**: Configurable single-worker (`single_riverlands`, `single_world`), per-region (`per_region`), per-ZOI (`per_zoi`), and mixed ZOI + City Node OBB + POI cut-out (`per_zoi_nodes_and_pois`) topologies with seamless cross-border player handovers (`5,000 cm` buffer, `3`-sample dwell, `2,000 ms` cooldown, and `lease_epoch` fencing).
- **Web Admin and GM Studio**: Real-time `30 Hz` (`33 ms`) entity streaming, crisp `z=7` minimap tiles, 3-pass ZOI borders, continuous `9x` server clock and 360-day 4-season Verra Calendar (`Spring`, `Summer`, `Autumn`, `Winter`), per-worker tick-rate selector (`Dynamic`, `0 Hz` NPC pause, `10..60 Hz`), searchable player teleport and NPC spawn modals, and the 4-zone **Observability** workspace.
- **Stats, Talents, and Equipment**: Bytecode-compiled character and NPC stat graphs (`statprog` / `libaocstats`), item rarity interpolation (`Common` to `Artifact`), inventory and material bags, and weapon skill-tree progression across all 21 weapon lines up to level 25.
- **Combat, Class Abilities, and Status Effects**: Class abilities, weapon combo finishers, magic weapon scaling, projectile flight timing, burning/bleed/poison damage-over-time ticks, stat debuffs (`Shaken`, `Volatile`, `Demoralized`, `Blinded`), and movement slows/snares (`MoveSpeed`, `Slow`, `Snare`, `Chilled`).
- **Navigation and Creature AI**: Chunked Recast/Detour navigation mesh (`area_nav.bin`) with town platform $Z$-floor rescue (`town_platforms.bin`), sight and hearing aggro, `140 m` leash rules, and `429` animated creature abilities across Beast, Humanoid, Monster, and Cultist families.
- **Mounts**: Ground, aquatic, and flying mounts with road speed buffs (`roads_levels.tsv`).
- **Gathering, Hunting, and Professions**: Authentic client-extracted and PCAP-backed gatherable foliage and resource nodes (`region_foliage.tsv`, `pcap_resource_spawners.tsv`), mining, logging, herbalism, hunting, processing jobs, and crafting at town stations with artisan gear bonuses.
- **Consumables and Currency**: Scrolls and elixirs with stat/artisan buffs, all six Glint tiers (including right-click 10-to-1 conversion), NPC vendors (buying, selling, repairing), and persistent item/material storage with the Storage Clerk.
- **City Nodes and Towns**: Live settlement progression across stages `0..6` (`Wilderness` to `Metropolis`, with Tier 5 and Tier 6 falling back to the Tier 4 populated layout), town scenery, NPCs, and crafting stations across the five major towns.
- **Quests and Map Markers**: NPC dialogue windows, quest offers, journal and tracker progress, world quest objects (bodies, crates, roots, building sites), item hand-ins during conversations, completion rewards, and map pins/areas for active objectives.
- **Embersprings**: Discovering Embersprings, attuning to a home shrine, and respawning after death.

### In Progress

- **Node Construction Sites**: Building sites below Village stage (`Stage 3`) and Wilderness camp progression.
- **Combat Reactions**: Class and weapon finisher reactions (Bard Resonance and melodies, wand/axe finisher debuffs, and cross-class reaction verification).
- **Construction and Trade Crates**: Packing crates at stations, carrying crates in the cargo slot, and dropping crates into the item bag (ground placement and building site turn-in are next).
- **Summoner Pets**: Summoning and commanding the Guardian, Hunter, and Mystic pets (follow, stances, 10 pet abilities, and `40 m` leash recall).
- **Multiplayer Visibility**: Showing nearby players within `150 m` (walking, standing, and cross-boundary AOI replication, with mounted riders, live gear/vital updates, and player-on-player abilities in progress).

### Planned

- **Quests**: NPC objective speech bubbles, choose-one reward selection windows, dedicated Quest bag, tag/rarity item hand-ins, Destiny progression, group quests, and commission boards.
- **Consumables and Inventory**: Food and drink regeneration meals, healing potions and salves, buff persistence across logout, and stack merging/splitting.
- **Crowd Control and Mob AI**: Hard crowd control on mobs (roots, stuns, knockbacks, sleeps), pack link aggro, boss-phase behavior-tree conditions, and flying creatures.
- **World Simulation**: Node-level-dependent wilderness spawns, lava damage, dynamic weather and world events, treasure hunts, and fishing.
- **Civic and Social Systems**: Full citizenship, mayoral elections, node taxes, vassal node hierarchies, player trading, marketplaces, guilds, parties, raids, chat, mail, caravans, and node sieges.

---

## Milestones

1. **1 - Network Protocol** -- `[Done]` Client-server handshake, packet framing, and actor replication.
2. **2 - Login** -- `[Done]` Desktop launcher, accounts, character creation, and character selection.
3. **3 - Into the World** -- `[Done]` Entering Verra as your character with persistent state across logins.
4. **4 - Stats** -- `[Done]` Authentic stat formulas updated dynamically as gear, buffs, and levels change.
5. **5 - Mounts** -- `[Done]` Ground, flying, and swimming mounts, sprinting, and road speed tiers.
6. **6 - Progression and Items** -- `[In Progress]` XP, levels, talents, action bars, bags, item rarities, weapon XP, scrolls, and elixirs. Remaining: stack splitting/merging, food, and potions.
7. **7 - Combat and Abilities** -- `[In Progress]` Class abilities, weapon combos and finishers, debuffs, slows, death/respawn, and Summoner pets. Remaining: hard crowd control on mobs.
8. **8 - Gathering and Professions** -- `[In Progress]` Mining, logging, herbalism, hunting, processing, and crafting at town stations. Remaining: fishing and additional artisan professions.
9. **9 - Living World** -- `[In Progress]` Streaming navigation mesh and animated mobs across Beast, Humanoid, Monster, and Cultist families with loot and respawns. Remaining: flying mobs and day/night spawn tables.
10. **10 - City Nodes** -- `[In Progress]` Settlement levels (`0..6`), XP, and the five major towns with NPCs, buildings, and stations. Remaining: full citizenship, elections, taxes, and vassal rules.
11. **11 - Quests** -- `[In Progress]` NPC conversations, quest offers, objectives, rewards, and map markers for starting-area quests. Remaining: choose-one reward windows, group quests, and dynamic events.
12. **12 - Economy** -- `[In Progress]` Gold/silver/copper, Glint, NPC vendors, crates, and warehouse storage. Remaining: node currencies, marketplaces, and caravan trade.
13. **13 - Social** -- `[In Progress]` Players within `150 m` see each other's characters moving in the world. Remaining: live mount/gear/vital replication to observers, guilds, parties, chat, mail, and player trading.
14. **14 - Node Sieges** -- `[Planned]` Node sieges and territory wars.
15. **15 - Caravans** -- `[Planned]` Caravans and regional trade routes.
16. **16 - World Events** -- `[Planned]` Corruption events and dynamic world events.
