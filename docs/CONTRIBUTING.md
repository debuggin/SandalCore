# Contributing to SandalCore

**Related Guides**: [Root Overview](../README.md) | [Security Policy](SECURITY.md) | [Operational Runbook](RUNBOOK.md) | [Wire Protocol Specifications](reference/wire-specs/README.md)

Thank you for contributing to SandalCore. SandalCore is an independent, clean-room server emulator compatible with the Ashes of Creation client, developed for preservation and educational research.

---

## Clean-Room and Safety Rules

All contributions must follow these rules:

1. **No Proprietary Game Assets**: Never commit game client binaries, `.pak` / `.utoc` / `.ucas` files, copyrighted audio, 3D meshes, textures, or extracted `content.db` / `.tsv` / `.bin` artifacts.
2. **No Unreal Engine Source Code**: Never copy code from the proprietary Unreal Engine source tree into this repository.
3. **No Personal Data or Secrets**: Never include personal names, local machine paths, external IP or MAC addresses, credentials, private keys, or session tokens in code, comments, tests, or pull requests.
4. **One Feature or Fix per Pull Request**: Keep each pull request focused on a single feature or bugfix so it builds and passes all tests independently.
5. **License**: All contributions are licensed under the **GNU Affero General Public License v3.0 (`AGPL-3.0-only`)** together with the terms in [`NOTICE`](../NOTICE).
6. **Self-Contained Documentation and Comments**: Refer only to files, symbols, and behavior inside this repository. Do not reference private notes or external ticket IDs.

---

## Development Workflow

### Building and Running Tests

Before opening a pull request, make sure your changes build cleanly and pass the automated verification suites.

If you have not extracted game data yet, run `.\sandal.ps1 extract` (or `.\start.bat`, which extracts game data automatically). Everything written to `build/`, `dist/`, and `deploy/` is ignored by Git and must never be committed.

1. **Static Repository and Asset Safety Check**:
   ```powershell
   pwsh -NoProfile -File scripts/checks/public_check.ps1
   ```
2. **Unit Test Suites (Server, Control Plane, Launcher, and Web Admin)**:
   ```powershell
   .\sandal.ps1 test
   ```
3. **Live End-to-End API and Play-Flow Verification**:
   ```powershell
   pwsh -NoProfile -File scripts/test_functional_apis.ps1
   ```

### Code Organization

Place new features in dedicated modules rather than growing large shared files:

- In `src/server/world/`, split player session features into bounded implementation files (`src/server/world/session/player_session_<feature>.cpp`), declare RPC registration helpers in `src/server/world/session/player_session.h`, and list `.cpp` files directly in `src/server/world/CMakeLists.txt` (never use `.inc` fragment files).
- Place zone server and library unit tests in `src/server/world/tests/` and `src/tests/cpp/`.
- Keep source files concise and well-structured (`<= 800` lines per file, `<= 60` lines per function).

### Commit Messages

Format commit subjects using **Conventional Commits** (`<type>(<scope>): <imperative summary>`, `<= 72` characters):

- `feat(world): implement summoner pet stance commands`
- `fix(gateway): validate ICS session ticket expiration`
- `docs(runbook): document multi-worker .env partition modes`

Explain *why* a non-obvious protocol or architectural change was made in the commit body.

---

## Submitting a Pull Request

1. Create your branch from the current `main` branch (`git checkout -b <branch>`).
2. Rebase onto `main` rather than merging `main` into your branch so history stays linear.
3. Run the three verification commands above and confirm zero warnings or failures.
4. Review your `git diff` to verify no secrets, local file paths, or extracted game assets are staged.
5. Open a pull request using the [Pull Request Template](../.github/PULL_REQUEST_TEMPLATE.md) and describe what changed and why.

---

## Reporting Bugs and Security Issues

- **Bug Reports**: Open an issue with the [Bug Report Template](../.github/ISSUE_TEMPLATE/bug_report.yml), including reproduction steps, commit hash, and sanitized logs.
- **Security Vulnerabilities**: Do not post security vulnerabilities in public issues. Follow the private reporting instructions in [`SECURITY.md`](SECURITY.md).
