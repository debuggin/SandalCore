## Description
<!-- Provide a concise description of what this PR does and why. -->

## Related Issues
<!-- Link relevant issues: e.g. Fixes #123, Closes #456 -->

## Changes Made
- 

## Scope
<!-- One feature or one fix per PR. Larger work goes into several PRs. -->
- [ ] This PR holds one feature or one fix.
- [ ] This PR is rebased on the current `main` (no merge commits from `main`), and its GitHub Actions checks pass.
- [ ] If I have other open PRs: this one applies cleanly on `main` and on top of them, one after another, in any order they may be merged (or it names the PR it depends on).
- [ ] New code is modular: the feature lives in files of its own (`src/server/world/session/player_<feature>.cpp`, `src/server/world/tests/`, `src/tests/cpp/`) and is registered the way existing features are, with as few edits to shared files as possible.

## Game Data
- [ ] This PR does not change the game data, or it says here what changes (the extractor, `contentc` or its version), so players know they need a data update.

## Verification & Testing
<!-- Describe how you verified these changes. Include test commands and output summary. -->
- [ ] Go tests: `cd src/server/services && go test ./...`
- [ ] C++ `world` & stack unit tests (`src/server/world/tests/` & `src/tests/cpp/`): `pwsh -File sandal.ps1 test`
- [ ] Functional API & play-flow tests: `pwsh -File scripts/test_functional_apis.ps1`
- [ ] PowerShell script syntax: verified clean execution

## Checklist
- [ ] My code adheres to the project guidelines in [CONTRIBUTING.md](../docs/CONTRIBUTING.md).
- [ ] Code comments, commit messages and this description only refer to what is in this repository: no ids or numbers of outside recordings or logs, no local file paths, no private notes.
- [ ] I have not included proprietary assets, game binaries, or Unreal Engine source files.
- [ ] I have verified that no personal data, private IPs, credentials, or tokens are present in diffs.
- [ ] All new and existing tests pass locally.
