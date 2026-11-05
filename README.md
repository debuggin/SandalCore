# SandalCore

An independent, open-source server emulator compatible with the Ashes of Creation client, developed by Cyb.

> [!IMPORTANT]
> **Not affiliated with Intrepid Studios.** SandalCore is an independent project. It is not affiliated with, sponsored by or endorsed by Intrepid Studios, Inc. *Ashes of Creation* is a trademark of Intrepid Studios, Inc., named here only to say which game client this server works with. The repository contains no game client, game files or Intrepid code: the data the servers need is built on your PC from your own installed copy of the game.

> [!WARNING]
> **Work in Progress**: SandalCore is **NOT** a final product. It is under active and consistent development; expect bugs, incomplete features, and ongoing protocol refinements. Please refer to [ROADMAP.md](docs/ROADMAP.md) for milestone progress, [KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) for current known limitations and workarounds, and [PATCH_NOTES.md](docs/PATCH_NOTES.md) for recent changes.

## Documentation

- [`docs/ROADMAP.md`](docs/ROADMAP.md): 16-phase milestone status, achieved features, and remaining work.
- [`docs/KNOWN_ISSUES.md`](docs/KNOWN_ISSUES.md): Current gameplay limitations, server constraints, and workarounds.
- [`docs/PATCH_NOTES.md`](docs/PATCH_NOTES.md): Dated changelog of features and fixes.
- [`docs/CONTRIBUTING.md`](docs/CONTRIBUTING.md): Branch conventions, clean-room guidelines, and test requirements.
- [`docs/SECURITY.md`](docs/SECURITY.md): Private vulnerability reporting policy.
- [`docs/reference/wire-specs/README.md`](docs/reference/wire-specs/README.md): Protocol and binary format wire specifications.

---

## Architecture & 11-Container Stack

SandalCore runs an 11-container Linux server stack inside WSL2 Docker ([`src/deploy/compose.yml`](src/deploy/compose.yml) and [`src/deploy/envoy/envoy.yaml`](src/deploy/envoy/envoy.yaml)) paired with the Windows desktop launcher **`SandalLauncher`** (`dist\launcher\SandalLauncher.exe` / `dist\launch_launcher.bat`), which serves the client session token in-memory over UDP `19021` via `LocalTetherListener`. All C++ (`world`) and Go (`gateway`, `auth`, `character`, `citynode`, `quest`, `mesh`, `meshgw`) server binaries compile inside multi-stage Linux Docker containers ([`src/deploy/docker/cpp.Dockerfile`](src/deploy/docker/cpp.Dockerfile) and [`src/deploy/docker/go.Dockerfile`](src/deploy/docker/go.Dockerfile)), while `world` serves authoritative 30 Hz UDP replication directly to the Ashes of Creation client.

```mermaid
flowchart LR
    subgraph host_win["Windows Host"]
        launcher["SandalLauncher (SandalLauncher.exe + LocalTetherListener :19021/udp)"]
        client["Ashes of Creation Client (AOCClient-Win64-Shipping.exe)"]
        browser["Operator Browser (Web Admin)"]
    end

    subgraph edge_net["WSL2 Docker — Edge Tier (edge_net)"]
        envoy["ingress-proxy (Envoy v1.31.2)"]
        webadmin["web-admin (Nginx + Admin UI/BFF :8088)"]
    end

    subgraph service_net["WSL2 Docker — Service Tier (service_net)"]
        gateway["gateway (Go :8081 / :50050 / :50061)"]
        auth["auth (Go :8081 / :50052)"]
        character["character (Go :50053)"]
        citynode["citynode (Go :50056)"]
        quest["quest (Go :50057)"]
        mesh["mesh (Go :50054)"]
        meshgw["meshgw (Go :8082 HTTP/SSE)"]
        world["world (C++23 30Hz :7777/udp / :50061-50062)"]
    end

    subgraph data_net["WSL2 Docker — Data Tier (data_net)"]
        postgres["postgres (PostgreSQL 17 :5432)"]
    end

    browser -->|"HTTP (:8080)"| envoy
    launcher -->|"HTTP/2 + gRPC (:8080, :9090)"| envoy
    client -->|"Tether (:19021/udp)"| launcher
    client -->|"ICS TLS (:50061)"| envoy
    client -->|"UE5 Replication (:7777/udp)"| world

    envoy -->|"HTTP (:8088)"| webadmin
    envoy -->|"HTTP/2, gRPC & ICS TLS"| gateway
    envoy -->|"HTTP/2 & gRPC"| auth
    envoy -->|"HTTP/SSE (:8082)"| meshgw

    gateway -->|"gRPC (:50052-50057)"| auth
    gateway -->|"gRPC"| character
    gateway -->|"gRPC"| mesh
    meshgw -->|"gRPC"| mesh
    meshgw -->|"gRPC"| citynode
    world -->|"gRPC (:50050-50057)"| gateway
    world -->|"gRPC"| character
    world -->|"gRPC"| citynode
    world -->|"gRPC"| quest
    world -->|"gRPC"| mesh

    auth -->|"TCP (:5432)"| postgres
    character -->|"TCP (:5432)"| postgres
    citynode -->|"TCP (:5432)"| postgres
    quest -->|"TCP (:5432)"| postgres
```

| # | Service (`container_name`) | Runtime | Ports (Host / Container) | Role |
|---:|---|---|---|---|
| 1 | `ingress-proxy` (`aoc-ingress-proxy`) | Envoy v1.31.2 | `:8080` HTTP/2 & Web Admin UI, `:9090` gRPC ingress, `:50061` ICS TLS, `:9901` Envoy admin | Edge reverse proxy routing browser, launcher, and client traffic across `edge_net` and `service_net` |
| 2 | `gateway` (`aoc-gateway`) | Go | `:8081` HTTP, `:50050` gRPC, `:50061` ICS TLS | Client session & ICS TLS gateway, character screen, and world handoff tickets |
| 3 | `auth` (`aoc-auth`) | Go | `:8081` HTTP, `:50052` gRPC | Account registration, login, and session token validation |
| 4 | `character` (`aoc-character`) | Go | `:50053` gRPC | Character creation, inventory, equipment, starting kits, and persistence |
| 5 | `citynode` (`aoc-citynode`) | Go | `:50056` gRPC | Intrepid Node progression, citizenship, workshops, and tax/mayoral state |
| 6 | `quest` (`aoc-quest`) | Go | `:50057` gRPC | Quest state machine, objectives, and dialogue progression |
| 7 | `mesh` (`aoc-mesh`) | Go | `:50054` gRPC | World zone registry, server boundary authority, and handover coordinator |
| 8 | `meshgw` (`aoc-meshgw`) | Go | `:8082` HTTP/SSE | Mesh HTTP/SSE gateway streaming live node, world, and GM state |
| 9 | `web-admin` (`aoc-web-admin`) | Nginx + Admin UI/BFF | `http://127.0.0.1:8080/` via Envoy (`:8088` internal) | Browser control plane for realm status, accounts, nodes, and world telemetry |
| 10 | `world` (`aoc-world`) | C++23 | `127.0.0.1:7777/udp`, `:50061` / `:50062` gRPC | Authoritative 30 Hz world simulation, replication, combat, NPC AI, and navmesh runtime |
| 11 | `postgres` (`aoc-postgres`) | PostgreSQL 17 | `127.0.0.1:5432` | Relational database isolated on `data_net` |

---

## What Works Today

SandalCore provides an independent server emulation stack for the Ashes of Creation client: the 11-container backend (`ingress-proxy`, the Go services `gateway`, `auth`, `character`, `citynode`, `quest`, `mesh`, `meshgw`, `web-admin`, `postgres`, and the C++23 world server `world`) together with the Windows desktop launcher **`SandalLauncher`** (`dist\launcher\SandalLauncher.exe` / `dist\launch_launcher.bat`, which serves the client session token in-memory via `LocalTetherListener` on UDP `19021`).

- **Protocol & Network Transport**: Full UE5 / IntrepidNetDriver handshake, stateless connect, packet framing, bunch replication, and dynamic NetGUID allocation.
- **Authentication & Character Management**: gRPC and REST-based authentication services, account profiles, character creation with full visual appearance customization serialization, and character select lobby transitions.
- **World Bootstrap & Replication**: Seamless map loading into `/Game/Levels/Verra_World_Master`, actor creation channels, possession handshakes, and player controller state replication.
- **Authoritative Stats Engine**: Core attribute formulas, expression evaluation graph, compiled stat program bytecode, and dynamic recalculation of stats upon equipment changes.
- **Progression, Items, and Talents**: Adventuring XP curves, level thresholds, skill tree talent acquisition, action bar persistence, inventory storage, and consumable item effects.
- **Complete Combat & Ability Runtime**: Authoritative ability pipeline supporting all retail classes, cast validation steps, cooldown ledgers, projectile trajectories, hit confirmation RPCs, floating combat numbers, shields, songs, buff/debuff effect stacks, weapon combos with their finishers, weapon swaps, and weapons that scale with the power they are meant for (Physical or Magical).
- **Living World & Navigation**: Chunked navmesh windows, Riverlands region partitioning, NPC actor spawning, creatures that notice players by hearing and sight and give up 140 m from where the fight began, wolves, bears, ravens, goblins and Grems that fight with their own attacks and animations, combat engagement, loot drops (rolled afresh on every server start), and respawn cycles.
- **Mounts & Traversal**: Ground, flying, and aquatic mount summoning, riding possession, mount combat mechanics, sprint/dash abilities, and road speed modifiers.
- **City Node Civilization**: Background `CityNodeService`, node experience accrual, level transitions, town layout replication, the real floor of the five towns with a safety net under it and their buildings in the walking map, the towns' people dressed and named from the game's own records, the walkers among them walking their routes, and the citizenship and mayor data the client reads (the citizenship and mayor rules are still to come).
- **Launcher (`SandalLauncher`)**: `dist\launcher\SandalLauncher.exe` (`dist\launch_launcher.bat`) presents a unified single semi-transparent card (`SERVER` selector, `SAVED LOGIN` selector + `Delete`, `USERNAME`, `EMAIL`, `PASSWORD`, `Remember login`, **`PLAY`**, **`Create Account`**, **`Temp Account`**, and **`Client Path...`**) over 3 randomized backgrounds (`launcher_background_0..2.jpg`). It discovers Steam libraries across all drives (`Registry` + fixed drives + `steamapps\libraryfolders.vdf`), auto-selects when a single installation is found or opens `Client Path...` when multiple installations (such as Retail + Playtest) or none are found, validates local TLS certificates via `X509ChainPolicy.CustomTrustStore`, and when you press **`PLAY`** authenticates first (halting with an error banner on invalid credentials) before launching `AOC\Binaries\Win64\AOCClient-Win64-Shipping.exe` with `-LauncherTetherPort=19021 -UseEOS=false -NoEAC`, serving `ActiveAccessToken` and the `127.0.0.1:50061` ICS TLS endpoint directly from memory over UDP `19021` via `LocalTetherListener` (never writing a token file to disk). Normal **`Create Account`** requires a valid email and checks username/email uniqueness (`409 Conflict`), while **`Temp Account`** creates an ephemeral account with an auto-generated email that is never saved to `LauncherProfiles.json` and is deleted immediately on logout or exit.
- **Gathering & Professions**: the 22 gathering professions, node harvesting (mining, lumberjacking and herbalism), the Riverlands' trees, plants and stones that grow back, huntable animals, resource bags and the pick-up popup.
- **Conversations & Quests**: the NPC talking window with the game's conversation data (lines a character does not qualify for are hidden), quest givers that offer their quests, accepting, the journal and the tracker, abandoning, quests kept across logins, objectives that count, the objects of the quests that you search and use (bodies, crates, roots, building sites), the hand-in with its rewards (items, experience and Glint), and the first quests of the starting area on both ways, with map pins for the objectives of active quests and a mark over the people who have a quest to take.
- **Merchants, Currency & Glint**: gold, silver and copper; buying, selling, repairing and destroying items at each town's general merchant; the Black Market, the caravan salesman and the sheriff's shops; Glint in its six kinds, paid for kills and rewards in kinds, and ten of a kind turned into one of the next with a right-click.
- **Crafting**: the crafting and processing stations of every Camp, Village and Town (they follow their node's level), processing jobs that are started, queued and collected with their items and experience, crafting at a station, the quick make, and recipes a game master can teach.
- **Item Rarities**: rarities for resources, weapons and armor, from creatures, harvests, shops and crafting; the game master's `give-item` can name a rarity.
- **Storage**: the Storage Clerk's free item storage and the town's materials storage, kept across logins.
- **Embersprings**: the 24 Embersprings of the Riverlands: discovering them, attuning to one, and choosing where to respawn.
- **Quiet Requests**: tutorials that stay done, the active and hover targets, the PvP flag, closed loot windows and the map marker.
- **Game Master & Admin Tools**: `worldctl` (teleport, speed, money and items, stations, crafting, tutorials, creature actions and a live watch of the world stream), `nodectl` (city node levels and experience), `questctl` (quests), and the `web-admin` browser UI at `http://127.0.0.1:8080/`.

---

## Build and run (Windows)

You need a Windows 10/11 x64 PC with your own installed copy of Ashes of Creation. SandalCore contains no game data: the first run reads your installed client (the files are only read, nothing in the game folder is changed) and extracts the data the servers need.

### 1. What to install

All C++ (`world`) and Go (`auth`, `gateway`, `character`, `citynode`, `quest`, `mesh`, `meshgw`) server binaries compile inside multi-stage Linux Docker containers ([`src/deploy/docker/cpp.Dockerfile`](src/deploy/docker/cpp.Dockerfile) and [`src/deploy/docker/go.Dockerfile`](src/deploy/docker/go.Dockerfile)), while **`SandalLauncher`** and the game-data extractors compile on Windows through the `.NET 10 SDK`. No host C++ or Go compiler toolchain is required on Windows.

| What | Version | What it is for |
|---|---|---|
| **Ashes of Creation** (Steam) | the build of 2026-01-29 | the source of the game data; it has to be installed on this PC |
| **WSL2 + Docker Engine / Docker CLI** | current | builds and runs all 11 Linux server containers (`aoc-postgres`, `aoc-ingress-proxy`, 7 Go services, `aoc-web-admin`, and `aoc-world`); provisioned via `pwsh -File sandal.ps1 install-wsl-docker` (detects an existing Docker daemon or installs standalone WSL2 Docker Engine across Ubuntu, Debian, Fedora, RHEL/Alma/Rocky, Arch, openSUSE, and Alpine distros, and sets `.wslconfig` `instanceIdleTimeout=-1` and `vmIdleTimeout=-1` non-destructively) |
| **.NET 10 SDK** | 10.0 (any 10.x; with the 10.0 runtime) | builds and runs `SandalLauncher.exe` (`src/clients/launcher/SandalLauncher.csproj`, including in-memory `LocalTetherListener` on UDP `19021`) and the game data extractors (`src/tools/aoc-extractor`, `src/tools/mapc`, `src/tools/itemnames`, `src/tools/npcabilities`), restoring NuGet dependencies (including `CUE4Parse`) automatically |
| **Git** | any current version | clones and updates the repository (and provides OpenSSL for local certificate generation) |

Disk: allow about 10 GB free for the extracted game data (`1.5 GB`) and container images. Memory: at least 6 GB free while the game data is extracted (the map reader peaks near 4 GB).

### 2. The commands, in this order

1. `pwsh -File scripts\install_dependencies.ps1` (or `-CheckOnly` to audit host prerequisites; `pwsh -File sandal.ps1 install-wsl-docker` provisions WSL2 + Docker Engine if not yet configured).
2. `start.bat` (or `pwsh -File sandal.ps1 run` / `pwsh -File sandal.ps1 up -Detach`): extracts the game data from your installed client if needed (about 10 minutes on the first run; seconds on later runs), generates your local ECDSA P-256 CA and SAN leaf certificates in `dist\certs\` and `deploy\certs\`, builds and starts the 11-container Linux server stack in WSL2 (`deploy/compose.yml` / `deploy/up.ps1`), and opens **`SandalLauncher`** (`dist\launcher\SandalLauncher.exe`). Create an account (**`Create Account`** with a valid email, or **`Temp Account`** for an ephemeral session) or enter your credentials (tick **`Remember login`** to save to `LauncherProfiles.json` / Windows Credential Manager) and press **`PLAY`**, which authenticates first and launches `AOC\Binaries\Win64\AOCClient-Win64-Shipping.exe` with `-LauncherTetherPort=19021 -UseEOS=false -NoEAC`. Use `build.bat` (or `pwsh -File sandal.ps1 build`) to build the stack and Windows launcher without starting, and `stop.bat` (or `pwsh -File sandal.ps1 down`) to stop the stack. (`start.bat`, `stop.bat`, and `build.bat` delegate through `scripts\ensure_pwsh.bat`, which checks for PowerShell 7 (`pwsh`) and prompts to install the latest PowerShell 7 via `winget` if not present.)

To extract only the game data on its own, run `pwsh -File sandal.ps1 extract` (or `pwsh -File scripts\extract_game_data.ps1`).

### 3. The game data (`scripts\extract_game_data.ps1`)

**Where the game is found**, in this order: `scripts\extract_game_data.ps1 -GameRoot "D:\...\Ashes of Creation\Game"`, the environment variable `GAME_ROOT`, `game_executable` in `src\config\paths.json` or `src\config\paths.local.json`, and universal Steam library discovery across all drives (`Registry`, fixed drives, and `steamapps\libraryfolders.vdf`). In `SandalLauncher`, if exactly 1 client installation is discovered it is selected automatically; if multiple installations (for example Retail + Playtest) exist or none are found automatically, the **`Client Path...`** modal opens so you can select or browse to the desired installation (persisted to `dist\config\paths.json`). The game folder is the one that holds `AOCClient.exe` (`AOC\Binaries\Win64\AOCClient-Win64-Shipping.exe`) and the `AOC` folder; with Steam it is `...\steamapps\common\Ashes of Creation\Game`. `-GameRoot` counts for that one run only. If your game is in an unusual place, set `GAME_ROOT` once, with `setx GAME_ROOT "D:\...\Ashes of Creation\Game"`, and open a new window: then `scripts\extract_game_data.ps1`, `sandal.ps1`, `start.bat`, and `SandalLauncher` all find it.

**What it makes** (all of it git-ignored; [`.gitignore`](.gitignore) lists the generated sources):
- `dist\content\content.db` (323 MB): the game numbers the servers use (`contentc`, from the client's `CacheDB.dbc`);
- `dist\content\*.tsv`: mount speeds and riders, profession curves, NPC motion, foliage and roads (`src\tools\mapc`, from the game's containers, and `contentc roads`);
- `dist\content\item_names.tsv`: the words the Glint kinds and some quest items are named with (`src\tools\itemnames`, from the game's containers);
- `dist\content\npc_abilities.tsv`, `npc_montages.tsv`, `npc_targeting.tsv`, `npc_senses.tsv` and `npc_aggro_curves.tsv`: what the creatures attack with, how their attack animations carry them, where each hit lands, and what each creature notices and how far it follows (`src\tools\npcabilities`, from the game's containers and `content.db`);
- `dist\content\npc_looks.tsv`, `npc_appearances.tsv`, `npc_names.tsv` and `npc_trees.tsv`: how the people (humanoid NPCs) look, what they are called, and whether they wander or patrol (`src\tools\npcabilities`, from the game's containers and `content.db`);
- `dist\content\town_platforms.bin` (4 MB): the floor of the five towns (`src\tools\mapc`, from two of the game's map packages);
- `dist\content\town_layout_items.tsv` (6 MB) and `town_layouts.bin` (13 MB): the buildings, walls and terraces of the five towns at the level each town starts at (a list made from the design data by `scripts\generators.ps1` / `src\tools\schemagen\gen_*.ps1` and `aoc-extractor`, then their collision from the game's packages by `src\tools\mapc`);
- `dist\content\area_nav.bin` (974 MB) and `area_geom.bin` (232 MB): the navigation mesh, with the towns' buildings in it (`src\tools\mapc` and `aoc_navbuild`);
- `dist\content\client_config\` and `zoi_chunks\`: the game's config files and its zone grid, which the PowerShell 7 schema/table generators in `src\tools\schemagen\` (`gen_*.ps1`, invoked via `scripts\generators.ps1`) read;
- the generated sources and test fixtures the build includes (for example `zoi_surface_table.h`, `node_constants.h`, `region_data.h`).

**The towns' levels.** The navigation mesh holds each of the five towns with the buildings of the level the server starts that town at, the one `src\config\seeds\citynode_seed.json` gives it (Joeva 4, Town; Halcyon 3, Village; the other three 0, Wilderness). If a GM changes a town's level in the game (`nodectl set-level`), the town in the game changes with it, but the navigation mesh keeps the buildings of the level it was made for: people can then walk where a new house stands or stand on a floor that is gone, until the mesh is made again. To make it for other levels, change the levels in the seed file and run `scripts\extract_game_data.ps1` again (the buildings and navigation mesh steps run, about 10 minutes). A level change in the game that swaps the mesh by itself is a later update.

**How long:** about 10 minutes the first time on a fast PC (24 threads, 31 GB: 10.3 minutes in all, of which the map read and the navigation mesh take 8); a slower PC takes longer. Every step is skipped when what it reads has not changed, so running it again takes about a second. `scripts\extract_game_data.ps1 -CheckOnly` says what would run, `-Only <step>` runs one step (the steps are `tools`, `config`, `curves`, `names`, `database`, `region`, `headers`, `fixtures`, `navbuild`, `roads`, `townfloors`, `townlayouts`, `npcabilities`, `navmesh`), `-Skip <step>` leaves one out and `-Force` runs everything again. The output of every program is kept in `build\.extract\`; `last_run.log` has all of it.

**Network:** the first build downloads NuGet packages for the .NET 10 tools (`aoc-extractor`, `SandalLauncher`, `mapc`) and container base layers / Go modules inside the Docker build. Nothing is downloaded at runtime after the initial build.

**After a game patch:** run `pwsh -File sandal.ps1 extract` (or `scripts\extract_game_data.ps1`) again. It sees that the game files changed and rebuilds everything that depends on them, about 10 minutes. The code is made for the client build of 2026-01-29 (`AOCClient-Win64-Shipping.exe` 235,606,624 bytes, `CacheDB.dbc` 144,085,052 bytes). With any other build the script prints a warning and goes on; the data can then be wrong or a step can fail.

**Navigation mesh (required for combat).** `world` starts its combat system only after `dist\content\area_nav.bin` loads; **without it every mob has 0 HP and damage does nothing** (see [KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md)). The `navmesh` step builds it; `scripts\extract_game_data.ps1 -Skip navmesh` leaves it out.

**What is in the repository.** `src/config/layouts/aoc.usmap` holds the names, types and layouts of the game's classes, structs and enums, as `src\libs\aocnet\protocol\schema\generated\` and `src\server\services\internal\designdb\exe_gen.go` do for the network protocol and the design database; none of the three holds game content.

### 4. If something goes wrong

| You see | Why | What to do |
|---|---|---|
| `The Ashes of Creation client was not found` | the game is not installed on this PC, or it sits in an unusual folder | install it (Steam), or run `scripts\extract_game_data.ps1 -GameRoot "D:\...\Ashes of Creation\Game"` (that run only), or set `GAME_ROOT` to that folder for good (`setx GAME_ROOT "D:\...\Ashes of Creation\Game"`, then open a new window) |
| `-GameRoot '...' is not the game's folder` | the folder has no `AOC\Content\Paks` or no `CacheDB.dbc` | give the folder that holds `AOCClient.exe` and `AOC` (the `Game` folder of the install), not the launcher folder; if the install is incomplete, let Steam verify the game files |
| `WARNING: this client is not the build this code was made for` | the game was patched, or it is another build | the script goes on; the data may be wrong or a step may fail (see "After a game patch") |
| `.NET 10 SDK` not found (`dotnet` missing) | the .NET 10 SDK is not installed or not on `PATH` | run `pwsh -File scripts\install_dependencies.ps1` (or `pwsh -File scripts\install_dependencies.ps1 -CheckOnly` to audit; `winget install Microsoft.DotNet.SDK.10`), open a new terminal window, and run `pwsh -File sandal.ps1 doctor` |
| Docker / WSL2 daemon unreachable | WSL2 Docker Engine is stopped or not yet provisioned | run `pwsh -File sandal.ps1 install-wsl-docker` (configures WSL2 Docker Engine and `.wslconfig` `instanceIdleTimeout=-1` / `vmIdleTimeout=-1`), then `pwsh -File sandal.ps1 doctor` |
| `ERROR in step '...'` | a step failed during extraction | read `build\.extract\last_run.log` and fix the cause; run `pwsh -File scripts\extract_game_data.ps1` again: finished steps are kept and the run continues at the failed one |
| `waiting: ...` in the map step | the map read waits until at least 6 GB of memory are free and the game, a build and `aoc_navbuild` are not running | close them; it continues by itself and gives up after 40 minutes |
| the CUE4Parse fetch fails | no network or no Git | connect and run again, or copy the folder yourself: `powershell -File src\tools\mapc\fetch_cue4parse.ps1 -From <folder with CUE4Parse>` |
| every mob has 0 HP and damage does nothing | `dist\content\area_nav.bin` is missing | run `pwsh -File scripts\extract_game_data.ps1` (without `-Skip navmesh`) |

### 5. Building from the command line

- **Full Stack + Launcher Build (`build.bat` / `sandal.ps1 build`)**:
  ```bat
  build.bat
  ```
  (Or run `pwsh -File sandal.ps1 build`.)
- **Containerized 11-container server stack (`deploy/compose.yml`)**:
  ```powershell
  pwsh -File sandal.ps1 up -Detach
  # or directly via Docker Compose:
  docker compose -f deploy/compose.yml --env-file deploy/.env up -d --build --wait
  ```
- **Desktop Launcher (`SandalLauncher`)**:
  ```powershell
  dotnet publish src/clients/launcher/SandalLauncher.csproj -c Release -o dist/launcher
  ```
- **Game Data Extractor & `.NET 10` Tools (`aoc-extractor`, `mapc`, `itemnames`, `npcabilities`)**:
  ```powershell
  dotnet build src/tools/aoc-extractor/AocExtractor.csproj -c Release
  dotnet build src/tools/mapc/mapc.csproj -c Release
  dotnet build src/tools/itemnames/itemnames.csproj -c Release
  dotnet build src/tools/npcabilities/npcabilities.csproj -c Release
  ```
- **Diagnostics & Test Suites**:
  ```powershell
  pwsh -File scripts\install_dependencies.ps1 -CheckOnly
  pwsh -File sandal.ps1 doctor
  pwsh -File sandal.ps1 status
  pwsh -File sandal.ps1 test
  pwsh -File scripts\test_functional_apis.ps1
  pwsh -File scripts\e2e_user_journey.ps1 -LiveDocker -IncludeDiagnostics
  ```

---

## Output Layout (`build/`, `dist/`, `deploy/`) & Root Entrypoints

SandalCore separates transient build intermediates from self-contained runtime bundles:

- **`build/`**: Transient build staging, extraction stamps (`build/.extract/`), C# `obj/` and `bin/` intermediates (`build/dotnet/`), generated fixtures (`build/gen/`), and test outputs (`build/tests/`). Safe to delete without losing extracted game data or certificates.
- **`dist/`**: Self-contained Windows runtime and launcher bundle (`dist/start_server.ps1`, `dist/stop_server.ps1`, `dist/launch_launcher.bat`, `dist/launcher/SandalLauncher.exe`, `dist/content/`, `dist/nav/`, `dist/certs/`, `dist/config/`). Works standalone on Windows if copied anywhere.
- **`deploy/`**: Self-contained 11-container stack (`deploy/compose.yml`, `deploy/up.ps1`, `deploy/down.ps1`, `deploy/.env`, `deploy/content/`, `deploy/nav/`, `deploy/certs/`, `deploy/config/`). Works standalone if copied anywhere.

Root entrypoints:
- `start.bat`, `stop.bat`, and `build.bat`: One-click Windows wrappers (`sandal.ps1 run`, `sandal.ps1 down`, and `sandal.ps1 build`) that delegate through `scripts\ensure_pwsh.bat` (which checks for PowerShell 7 `pwsh` and prompts to install the latest PowerShell 7 via `winget` if not present).
- `sandal.ps1`: Unified CLI (`doctor`, `status`, `up`, `down`, `extract`, `certs`, `build`, `deploy`, `test`, `run`, `install-wsl-docker`, `help`).
- `scripts\extract_game_data.ps1`, `scripts\generators.ps1` (`src\tools\schemagen\gen_*.ps1`), and `scripts\launch_all.ps1`: Game-data extraction, PowerShell 7 schema/table generation, and full local launch scripts.

---

## Running the Server

### 1. Quickstart (`start.bat` / `stop.bat` / `build.bat`, `sandal.ps1`, or `SandalLauncher.exe`)

```bat
start.bat
```
(Or run `pwsh -File sandal.ps1 run` / `pwsh -File sandal.ps1 up -Detach`, or launch `dist\launcher\SandalLauncher.exe` via `dist\launch_launcher.bat`.) Extracts game data from your Ashes of Creation install if needed, generates local ECDSA P-256 TLS certificates, starts the 11-container Linux stack in WSL2 (`deploy/compose.yml` / `deploy/up.ps1`), and opens **`SandalLauncher`** (`dist\launcher\SandalLauncher.exe`).

Inside **`SandalLauncher`** (single semi-transparent card with `SERVER` selector, `SAVED LOGIN` selector + `Delete`, `USERNAME`, `EMAIL`, `PASSWORD`, `Remember login`, **`PLAY`**, **`Create Account`**, **`Temp Account`**, and **`Client Path...`** over 3 randomized backgrounds `launcher_background_0..2.jpg`):
- **Client Discovery**: Scans Steam libraries across all drives (`Registry`, fixed drives, and `steamapps\libraryfolders.vdf`). Automatically selects the client if 1 installation is found, or opens the **`Client Path...`** modal if multiple installations (for example Retail + Playtest) exist or if no installation is found automatically.
- **Account Creation & Ephemeral Sessions**: Normal **`Create Account`** requires a valid email and checks that the username and email are not already in use (`409 Conflict`). **`Temp Account`** creates an ephemeral account with an auto-generated email that is never saved to `LauncherProfiles.json` and is deleted immediately on logout or exit. With **`Remember login`** ticked on normal accounts, saved logins are persisted to `LauncherProfiles.json` / Windows Credential Manager (`SandalCore/LauncherSignIn`) and can be managed or removed via the `SAVED LOGIN` selector + `Delete`.
- **Authenticated Launch & In-Memory Tether**: Pressing **`PLAY`** authenticates first (halting with an error banner if credentials are invalid) and launches `AOC\Binaries\Win64\AOCClient-Win64-Shipping.exe` with `-LauncherTetherPort=19021 -UseEOS=false -NoEAC` (no game files are modified), serving the session token (`ActiveAccessToken`) and `127.0.0.1:50061` ICS TLS endpoint directly from memory over UDP `19021` via `LocalTetherListener` (never writing a token file to disk).

To build the server containers and Windows launcher without starting them:
```bat
build.bat
```
(Or run `pwsh -File sandal.ps1 build`.)

To stop the stack:
```bat
stop.bat
```
(Or run `pwsh -File sandal.ps1 down`.)

### 2. WSL2 Docker Engine & Non-Destructive `.wslconfig` Management

`pwsh -File sandal.ps1 install-wsl-docker` (`scripts\lib\wsl-docker.ps1` and `scripts\lib\wsl-config.ps1`) manages your container runtime without requiring a commercial desktop GUI:
- **Automatic Daemon Detection & Multi-Distro Provisioning**: Automatically detects whether an existing Docker daemon/context is already active on the host, or provisions native WSL2 Docker Engine inside your chosen Linux distribution across **Ubuntu**, **Debian**, **Fedora**, **RHEL / AlmaLinux / Rocky Linux**, **Arch Linux**, **openSUSE**, and **Alpine Linux**.
- **Non-Destructive `%USERPROFILE%\.wslconfig` Persistence**: Ensures `[general] instanceIdleTimeout=-1` and `[wsl2] vmIdleTimeout=-1` are set so the WSL2 VM and 11-container stack never terminate when Windows terminal windows close, while preserving your existing `.wslconfig` section ordering, custom keys, inline comments, and whitespace.

### 3. Portable Deployment (`deploy/up.ps1` / `deploy/down.ps1`)

The staged `deploy/` directory is a self-contained 11-container stack (`deploy/compose.yml`, `deploy/up.ps1`, `deploy/down.ps1`, `deploy/.env`, `deploy/content/`, `deploy/nav/`, `deploy/certs/`, `deploy/config/`) that works standalone if copied to another folder or machine:

```powershell
pwsh -File deploy\up.ps1
pwsh -File deploy\down.ps1
```

### 4. Windows Client Launcher & Staged Assets (`dist/`)

The staged `dist/` directory holds the compiled Windows desktop launcher (`dist/launcher/SandalLauncher.exe`, `dist/launch_launcher.bat`), extracted game data (`dist/content/`, `dist/nav/`), generated ECDSA P-256 TLS certificates (`dist/certs/`), and merged runtime configuration (`dist/config/`):

```bat
dist\launch_launcher.bat
```

### 5. TLS Certificates

SandalCore ships no keys or certificates. On the first launch `scripts\sandal\certs.ps1` (`pwsh -File sandal.ps1 certs`) generates a local ECDSA P-256 CA (`dist\certs\ca.crt`, `ca.key`) and SAN leaf certificate (`server.crt`, `server.key`, `server.pem`) covering `localhost`, `127.0.0.1`, and any configured advertise IP or domain. `SandalLauncher` validates the server certificate directly against `ca.crt` via `X509ChainPolicy.CustomTrustStore` without modifying the Windows OS Root Certificate Store (no administrator prompt required).

Run `pwsh -File sandal.ps1 certs -Force` (`pwsh -File scripts\sandal\certs.ps1 -Force`) to regenerate certificates. Details: [`src/config/certs/README.md`](src/config/certs/README.md).

### 6. Local Configuration Overrides, Environment Variables & Network Ports

Base tracked configuration files in `src/config/` (`server.json`, `realm.json`, `paths.json`, `launcher.json`, `sandal.env.example`, `profiles/*.env.example`) use safe loopback defaults (`127.0.0.1`, placeholder tokens) and are never committed with secrets. Personal settings and secrets belong in gitignored override files:
- `src/config/*.local.json` (`server.local.json`, `realm.local.json`, `paths.local.json`)
- `src/config/sandal.local.env` and `deploy/.env`

These local overrides are automatically merged into `dist/` and `deploy/` when staging or launching.

Key runtime environment variables and settings include:
- `GAME_ROOT`: The folder of your installed Ashes of Creation client that holds `AOCClient.exe` and `AOC\` (for example `C:\Program Files (x86)\Steam\steamapps\common\Ashes of Creation\Game`). `scripts\extract_game_data.ps1` and the launchers use it before they search for the game.
- `AOC_CLIENT_CONFIG`: The game's `AOC\Config` folder (default `dist\content\client_config`, written by `scripts\extract_game_data.ps1`).
- `AOC_ZOI_CHUNKS`: The zone-of-influence chunk folder (default `dist\content\zoi_chunks`, written by `scripts\extract_game_data.ps1`).
- `AOC_CONTENT_DB`: Path to game content database fixtures (defaults to `dist\content` on host, `/app/content` inside containers).
- `AOC_MIRROR_LOGS_ROOT`: Log mirror destination (defaults to `dist\logs\mirror`).
- **Database Connection**: Configured via `deploy/compose.yml` (`deploy/.env`) or connection string (`postgres://aoc:<password>@127.0.0.1:5432/aoc?sslmode=disable`).
- **Launcher Path**: Configured in `src/config/paths.json` or `src/config/paths.local.json` (auto-detected from `GAME_ROOT`).
- **Network Ports (`deploy/compose.yml` & `src/deploy/envoy/envoy.yaml`)**:
  - `http://127.0.0.1:8080`: Envoy HTTP/2 edge ingress & `web-admin` browser UI (`/` -> `web-admin:8088`, `/api/auth` & `/login` -> `auth:8081`, `/api/gateway` -> `gateway:8081`, `/api/mesh` -> `meshgw:8082`)
  - `127.0.0.1:9090`: Envoy gRPC ingress (`auth.v1`, `character.v1`, `citynode.v1`, `quest.v1`, `mesh.v1`, `gateway.v1`, `ics.v1`, `worldadmin.v1`)
  - `127.0.0.1:50061`: Envoy ICS TLS edge listener (forwarded to `gateway:50061`)
  - `127.0.0.1:19021/udp`: `SandalLauncher` in-memory `LocalTetherListener` (serves `ActiveAccessToken` and `127.0.0.1:50061` ICS TLS endpoint directly from memory to `AOC\Binaries\Win64\AOCClient-Win64-Shipping.exe`, never writing a token file to disk)
  - `127.0.0.1:9901`: Envoy local admin interface
  - `127.0.0.1:7777/udp`: `world` (`aoc-world`) authoritative UE5 replication listener (`world --bind`)
  - `127.0.0.1:5432`: PostgreSQL 17 (`postgres` / `aoc-postgres`)

---

## Captures

SandalCore includes no packet captures. The community's own repository holds the plaintext captures of the live game that this project's protocol work started from: [AshesEmulatorTeam/PCAPRepo](https://github.com/AshesEmulatorTeam/PCAPRepo), captured by the Creation of Ashes (CoA) community. Thank you.

---

## Contributing

We welcome contributions from the community! Please read [`CONTRIBUTING.md`](docs/CONTRIBUTING.md) for our code of conduct, branch conventions, and testing requirements before opening a pull request.

---

## Reporting Bugs

- **Bug Reports**: Open a new issue using our [GitHub Bug Report Form](.github/ISSUE_TEMPLATE/bug_report.yml).
- **Security Vulnerabilities**: Do **not** open public issues for security concerns. Report them privately according to [`SECURITY.md`](docs/SECURITY.md).

---

## Credits

Developed by Cyb. Special thanks to Hatemost for building the foundation and CoA (Creation of Ashes) / the Ashes community who participated in capturing the PCAPs.

---

## License

This program is free software: you can redistribute it and/or modify it under the terms of the **GNU Affero General Public License version 3 (AGPL-3.0-only)** with additional terms under Section 7. See [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE) for the full license text and terms.

---

## Disclaimer

Not affiliated with, sponsored by or endorsed by Intrepid Studios, Inc.; Ashes of Creation is their trademark, named only to identify the compatible client; no game client, game files or Intrepid code included; the game data is built on your PC from your own legally obtained client; for preservation and education.
