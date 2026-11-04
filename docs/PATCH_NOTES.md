# Patch notes

## 2026-11-04: Summoner pets, multiplayer presence, mob debuffs, consumables and 30 Hz tick scaling

### New
- Summoner pets: the Guardian, Hunter and Mystic summon after their 8 s cast, follow the Summoner, fight using the
  owner's stats, take pet bar commands and stances (Attack, Follow, Stay, Go To within 40 m, Aggressive, Defensive,
  Passive, Dismiss), cast their ten pet abilities manually or in auto mode, and accept Summoner abilities cast on them.
- Multiplayer presence (stage A): players within 150 m of each other see each other's characters and `PlayerState`
  records in the world, standing, walking and stopping with their name, race, gender, level and equipped gear, and
  leaving other players' views on logout.
- Mob damage-over-time and stat debuffs: mobs take the Burning debuff (1 s fire ticks for 8 s, up to 24 s), Bleeding
  (1 s ticks for 6 s per attacker), Hemorrhage (0.5 s ticks for 5 s on a bleeding mob) and Poison (2 s ticks for 20 s,
  scaling with up to 10 stacks), plus Shaken, Volatile, Demoralized, Blinded, Shocked and Wounded. Debuffs persist
  while a mob walks home after leashing and re-engage once it reaches home.
- Consumables (scrolls and elixirs): using a scroll or elixir from the item bag consumes one from the stack, triggers
  the 1 s cooldown, and applies its stat and artisan rating buff for its duration (300 s to 10,800 s for scrolls,
  3,600 s for elixirs) at the item's quality, with one active scroll and one active elixir at a time.
- Weapon talent trees: extended finisher talents (Burning, Chilled, Volatile, Bleed, Snare, Wound, Stagger, Daze,
  Concussion, Heartseeker, Arrowstorm, Unstoppable, Swordmaster's Swiftness and Bullseye) fire on deadly finishing hits
  (25 % base roll plus 10 % per Proc Chance talent), and weapon damage talents (Damage Focus, Projectile Damage,
  Finisher Damage, Deadly Finisher Damage) scale hits for the active weapon across swaps.
- Crowded-area 30 Hz tick scaling: a fixed-rate server tick, round-robin view updates, shared mob replication bunches
  across observers, parallel view evaluation on up to 4 processor cores, and retail-paced movement acknowledgements
  sustain 30 updates per second with roughly twice as many simulated walking players in one area.

### Fixed
- Ranged weapon auto-attacks and class projectiles: Wand, Shortbow, Longbow and Book swings, plus Mock, Quick Wit,
  Jest, Sleep Dart, Throw, Salvo, Mend, Discordance (with 10 m bounces) and Firebolt/Frostbolt land when their
  projectiles reach the target and carry the character's class animation speed rate.
- Elevated platform mount riding: tracked player positions when standing on or beside a mount on an elevated base so
  mounting is no longer refused as out of range.
- Combat rolls, Guard and level-ups: seeded hit, block and critical rolls per server start; restored Stamina recovery
  after a quick tap of Guard; restored Health, Mana and Stamina on level-up; and kept Options menu choices across
  logins.
- Harvesting, crafting and data extraction: dropped overflow harvests even if the player disconnects immediately,
  routed GM vendor material purchases into material bags when the backpack is full, capped gathering channels and
  quantities against overflow, and skipped `navmesh` rebuilds in `scripts\extract_game_data.ps1` unless map geometry or
  city node inputs in `src/server/world` change.

## 2026-10-17: The towns' ground, the launcher and hardening

### New
- Solid ground in the five towns: Winstead, Joeva, New Aela, Halcyon and Miraleth have their real floor, read from the
  game's own town platforms. Logins, teleports, mount summons, dismounts and respawns inside a town land on its streets.
- A safety net under the towns: a player who falls far below a town is put back on its floor within a few seconds.
- Keep me signed in: a box on the launcher's sign-in form. A sign-in with the box ticked is saved in Windows Credential
  Manager (for your Windows account only, never in a file or a log). From then on the launcher signs in by itself at
  start: three seconds with Cancel, then the sign-in, retried for up to a minute while the servers start. A refused
  sign-in keeps the saved one and shows the form; Logout forgets it. It signs in by itself only against a server on
  this PC.
- LAUNCH GAME: after signing in, the player presses LAUNCH GAME to start the game. The launch script no longer starts
  the game, and a sign-in never starts it. While the game is open the button reads GAME RUNNING; if a game is already
  open it says so and starts nothing, so a second game client cannot be opened by mistake.
- The data extractor builds the towns' floor: a new step, `townfloors`, reads the two town platform maps of your own
  install into `dist\content\town_platforms.bin` (a few seconds).

### Fixed
- Logging in inside a town takes about 30 seconds instead of about two minutes: the server sends a town's buildings and
  streets during the login, before the character appears, so the client finds its floor.
- Launcher: the game path is saved only next to the launcher and only when it changes (it used to be written to three
  places, including the repository's own `src\config\paths.json`); Auto-Detect searches fixed drives only; sign-in requests
  time out.
- Launcher sign-in is stricter: the saved sign-in goes only to `127.0.0.1`, `localhost` or `::1`, written exactly
  like that; the host and port are read once per sign-in and used for every retry; a reply is limited to 64 KB, a
  request to 15 seconds, and a redirect is never followed; registry text and long folder names are read safely.
- Hardening, thanks to DuskyProjects for the bounds and overflow checks: the packet reader, packet framing, bunch
  headers and replication handles check sizes and numbers instead of reading past their end; so do the navigation mesh,
  geometry, road, city node and UDP address readers and the map tool's geometry reader. The per-account character limit
  holds even when two sessions create characters at the same moment. The sign-in API accepts exactly one JSON value per
  request, and the stat curves refuse non-finite keys and NaN lookups. The test client splits a burst of requests into
  valid packets under the packet size limit. The property-update bunch builder test, which failed before, passes.

## 2026-10-16: Gathering

### New
- Gathering professions: every character has the 22 gathering professions, with levels,
  XP and certification tiers.
- Node gathering: the Riverlands' mining, lumberjacking and herbalism nodes can be harvested,
  using the game's own quantity, XP and tool-wear values. Nodes respawn.
- World foliage: the painted trees, plants and stones of the Riverlands can be cut, picked
  and mined. They grow back in stages, and every player sees the same state.
- Hunting: huntable versions of animals spawn and are huntable.
- Resource bags: gathered items go to the resource bag; older harvests in the Backpack move
  there at login. When the bag is full, the rest drops on the ground.
- Pick-up popup: every harvest shows "Acquired [item] xN" with the item's icon.
- GM tools: artisan level scrolls and T3 gathering tools in the GM vendors.
- The client starts with -UseEOS=false; no game files are modified.

### Fixed
- Tree gathering now finishes.
- Timid creatures no longer attack on sight; they fight back when hit.
