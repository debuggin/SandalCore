# SandalLauncher Desktop Client & Host Tether (`src/clients/launcher`)

**Primary Diataxis Quadrant**: `Technical Reference` (`DOC-01`)
**Related Guides**: [Root Architecture Overview](../../../README.md) | [UI Style Guide & Design System](../../../docs/UI_STYLE_GUIDE.md) | [Configuration Reference](../../../docs/CONFIGURATION.md) | [Go Control-Plane Services Reference](../../server/services/README.md)

`src/clients/launcher` (`SandalLauncher`) is the cross-platform `.NET 10` (`net10.0`) and Avalonia 11.2 MVVM desktop launcher (`dist/launcher/SandalLauncher.exe`). Running `start.bat` (`sandal.ps1 up`) automatically creates a root `SandalLauncher.lnk` Windows shortcut pointing to `dist/launcher/SandalLauncher.exe` for one-click launch from the repository root. It handles HTTP authentication against `aoc-gateway` (`:8080`), runs the in-process Windows host UDP ARQ Tether responder (`LocalTetherListener` on `127.0.0.1:19021/udp`), discovers local *Ashes of Creation* installations across Steam libraries, and launches `AOCClient-Win64-Shipping.exe` with the exact network and certificate parameters required to connect to the SandalCore stack.

---

## 1. Source Architecture & Key Components

| File / Class | Responsibility |
|:---|:---|
| [`Program.cs`](Program.cs) | Entry point supporting both interactive Avalonia desktop GUI mode and headless CLI automation (`--headless`, `--email`, `--password`, `--register`, `--launch`). |
| [`ViewModels/MainWindowViewModel.cs`](ViewModels/MainWindowViewModel.cs) | MVVM state machine managing single-card glassmorphic login over 3 randomized Verra background themes, ephemeral `Temp Account` creation/cleanup, server health probes, and launch orchestration. |
| [`Services/LocalTetherListener.cs`](Services/LocalTetherListener.cs) | In-process UDP ARQ server bound to `127.0.0.1:19021/udp` on the Windows host that completes the `AskWindow` / `TellWindow` (`0x04`) and `LoginTicket` (`0x01` -> `0x02`) handshake with `AOCClient-Win64-Shipping.exe`. |
| [`Services/GatewayAuthService.cs`](Services/GatewayAuthService.cs) | HTTP client communicating with `aoc-gateway` (`:8080`) for `/v1/auth/register`, `/v1/auth/login`, `/v1/auth/temp-cleanup`, and `/v1/tether/ticket`. |
| [`Services/GameInstallLocator.cs`](Services/GameInstallLocator.cs) | Automatic game binary locator scanning `AOC_GAME_PATH`, Windows Registry keys, and multi-drive Steam `libraryfolders.vdf` manifests for `AOCClient-Win64-Shipping.exe`. |
| [`Services/SandalConfigLoader.cs`](Services/SandalConfigLoader.cs) | Loads root `.env` / `deploy/.env` settings, inspects `dist/certs/server.pem` for wildcard SANs (`*.<domain>`), and maps `localhost` / `127.0.0.1` to `local.<domain>` when a wildcard certificate is present. |

---

## 2. Core Protocol & Lifecycle Contracts

### 1. In-Process Windows Host UDP Tether (`127.0.0.1:19021/udp`)

`AOCClient-Win64-Shipping.exe` hardcodes its pre-login launcher tether check to `127.0.0.1` (Windows host loopback). Therefore, `LocalTetherListener` runs directly inside `SandalLauncher` on the Windows host (never inside a Linux Docker container):

- **ARQ `AskWindow` / `TellWindow` (`0x04`) Handshake**: When the UE5 client sends a reliable `AskWindow` frame (`HeaderFlags & 0x04`), `LocalTetherListener` echoes a `TellWindow` (`0x04`) response with `NextExpectedRecvSeq = (ushort)(_recvSeq + 1)` (`LocalTetherListener.cs`) so the client's `ReliableWindow` state machine advances cleanly to `Connected`.
- **Ticket Delivery (`0x01` -> `0x02`)**: Once the ARQ window is established, `LocalTetherListener` replies to opcode `0x01` (`RequestTicket`) with opcode `0x02` (`TicketResponse`) carrying the UTF-8 session ticket issued by `aoc-gateway`.

### 2. Native In-Game Character Creation (`0` Pre-Launch Stub Characters)

`SandalLauncher` never pre-creates stub characters before launching the game client. When an account has `0` characters in `aoc-postgres`, `AOCClient-Win64-Shipping.exe` connects over TLS 1.3 to `aoc-character` (`:50061` IntrepidLobbyService / ICS), receives an empty `CharacterList`, and enters the native UE5 in-game character creator (`CharacterCreate` RPC).

### 3. Exact `AOCClient-Win64-Shipping.exe` Command-Line Arguments

When the user clicks **Play** (or runs `--headless --launch`), `SandalLauncher` spawns `AOCClient-Win64-Shipping.exe` with the following exact arguments:

```text
-LauncherTetherPort=19021 -GatewayAddr=<host:8080> -IcsAddr=<host:50061> -UseEOS=false -NoEAC
```

Where `<host>` resolves to `127.0.0.1` by default, or `local.<domain>` when `dist/certs/server.pem` contains a wildcard SAN (`*.<domain>`).

### 4. Ephemeral `Temp Account` Lifecycle

Clicking **Temp Account** in the launcher UI provisions a one-click disposable test account (`temp_<random>@sandal.local`) and caches its state in `deploy/.temp_accounts.json`. On the next launcher startup or exit, `GatewayAuthService` calls `/v1/auth/temp-cleanup` to purge expired temporary accounts and their characters from `aoc-postgres`.

---

## 3. Build, Shortcut & Test Commands

`SandalLauncher` compiles out-of-tree into `build/dotnet/` and publishes to `dist/launcher/SandalLauncher.exe`. Running `.\start.bat` (`.\sandal.ps1 up`) automatically creates a root `SandalLauncher.lnk` Windows shortcut pointing to `dist/launcher/SandalLauncher.exe` for one-click launch from the repository root:

```powershell
# Build & publish SandalLauncher via the unified orchestrator
.\build.bat

# Start the server stack, generate root SandalLauncher.lnk -> dist/launcher/SandalLauncher.exe, and launch
.\start.bat

# Run the xUnit + Avalonia.Headless.XUnit test suite directly
dotnet test src/clients/launcher.Tests/SandalLauncher.Tests.csproj -c Release
```
