# Contributing to SandalCore

Thank you for your interest in contributing to SandalCore! SandalCore is an independent, open-source server emulator compatible with the Ashes of Creation client, developed for preservation and educational research.

---

## Code of Conduct & Safety Guidelines

To keep the repository clean, legal, and safe for everyone, all contributors must strictly adhere to the following rules:

1. **No Proprietary Game Assets**: Never submit game client binaries, game paks, copyrighted audio, 3D meshes, textures, or official game assets.
2. **No Unreal Engine Source Code**: Never copy code from the proprietary Unreal Engine source tree into this repository.
3. **No Personal Data or Secrets**: Never include personal identification, real names, personal file paths, non-loopback IP addresses, MAC addresses, credentials, private keys, or session tokens in code, comments, test fixtures, or pull request descriptions.
4. **One Feature or Fix per Pull Request**: A pull request holds one feature or one fix. Split larger work into several pull requests that each build and pass the tests on their own. Do not include refactors the change does not need.
5. **License Agreement**: All contributions are submitted under and licensed pursuant to the **GNU Affero General Public License version 3 (AGPL-3.0-only)** along with the additional terms specified in [`NOTICE`](../NOTICE).
6. **Only What Is in the Repository**: Code comments, commit messages and pull request descriptions refer only to what is in this repository. Do not cite ids or numbers of outside recordings or logs, local file paths or private notes.
7. **Several Pull Requests That Fit Together**: If you open more than one pull request, make sure they do not conflict when they are applied one by one. Base each on the current `main`; when one is merged, rebase the others onto the new `main` and check that they still build and pass the tests. If one depends on another, say so in its description.
8. **Modular Code**: Build each feature as a module as much as possible: new files of its own, a small entry point into the shared code, and as few edits to shared files as possible (see "Where New Code Goes"). Modular changes are easier to review, and they keep pull requests from conflicting with each other.
9. **Up to Date With `main`**: A pull request is based on the current `main` with a linear history: rebase your branch, do not merge `main` into it. When `main` moves on and your pull request conflicts or falls behind, rebase it yourself.

---

## Development Workflow

### Building and Testing

Before submitting changes, ensure your modifications compile cleanly and pass all test suites.

Run `pwsh -File sandal.ps1 extract` once first (`start.bat` runs this automatically): the build and the tests require the game data extracted from your installed Ashes of Creation client (`scripts\extract_game_data.ps1`). Everything written to `dist/`, `deploy/`, and `build/` (as well as the generated files listed in `.gitignore`) is git-ignored; never commit any extracted or generated game data.

- **Go Services** (in `src/server/services/`):
  ```bash
  go vet ./...
  go test ./...
  ```
- **Stack Verification & Functional API Tests**:
  Build the stack and run the verification & functional API test suites:
  ```powershell
  pwsh -File sandal.ps1 test
  pwsh -File scripts/test_functional_apis.ps1
  ```
- **PowerShell Scripts**:
  Verify scripts execute without syntax or parsing errors.

### Where New Code Goes

Put a new feature into source files of its own and register it the way the existing features do, instead of growing large shared files:

- The `world` server's player session splits features across bounded-context implementation files (`src/server/world/session/player_session_<feature>.cpp`), declares RPC registration helpers (`register_<feature>_rpcs`) in `src/server/world/session/player_session.h` (dispatched from `src/server/world/session/player_session_movement.cpp`), and lists source files directly in `src/server/world/CMakeLists.txt` (`0` `.inc` files).
- Tests live in dedicated test files under `src/server/world/tests/` and `src/tests/cpp/`.
- Structural file and function LOC bounds (`<= 800` LOC/file target, `<= 60` LOC/function target, `0` `.inc` files) are enforced by `verify-fitness-functions.ps1` and `scripts/checks/public_check.ps1`.

### Commit Messages

- Format commit subject lines using **Conventional Commits v1.0.0** (`<type>(<scope>): <imperative summary>`, `<= 72` characters), such as:
  - `feat(world): implement summoner pet stance commands`
  - `fix(gateway): validate ICS session ticket expiration`
  - `docs(readme): document content.db v41 auto-rebuild flow`
- Supported types include `feat`, `fix`, `refactor`, `perf`, `test`, `docs`, `build`, `ops`, `ci`, `chore`, and `revert`.
- Include the rationale (`WHY`) behind non-obvious architectural or protocol fixes in the commit body.

---

## Submitting Pull Requests

1. Fork the repository and create your branch from `main`.
2. Rebase onto the current `main`, then make sure the code compiles with zero warnings or errors and every test passes on your own machine (both commands under "Building and Testing"). The GitHub Actions checks on your pull request must pass too.
3. Review your diff thoroughly to verify no credentials, personal paths, or proprietary files were added.
4. Open a pull request using the project's [Pull Request Template](../.github/PULL_REQUEST_TEMPLATE.md).
5. Explain what changed and why, linking to relevant GitHub issues if applicable.
6. A pull request that is not based on the current `main`, fails a check, mixes unrelated changes or leaves the template's checklist unanswered is sent back with a request for changes. A pull request with no update for 14 days after such a request may be closed.

---

## Reporting Bugs & Security Vulnerabilities

- **Bug Reports**: Open an issue using our [GitHub Bug Report Form](../.github/ISSUE_TEMPLATE/bug_report.yml). Provide clear reproduction steps, commit hash, and sanitized logs.
- **Security Vulnerabilities**: Do **not** post security bugs to public issues. Please report security issues privately via GitHub's private vulnerability reporting feature as described in [`SECURITY.md`](./SECURITY.md).
