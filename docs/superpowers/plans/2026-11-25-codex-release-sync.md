# Codex 0.172.0 Release Sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to execute this integration sequentially; independent audits may run in parallel. Only the coordinator changes shared Git state.

**Goal:** Merge the latest official Codex release into the existing IM fork, preserve its behavior, and update the parent gitlink with verified source.

**Architecture:** Parent development stays on `main`; the submodule uses its existing `multi-ai/im-bridge` maintenance branch. Merge the peeled `rust-v0.172.0` commit `3d86e43b075adeba2193ec5c22ac92f55afd82e6`, retaining both histories. Keep user data, previous build artifacts and running applications unchanged.

**Tech Stack:** Git submodule, Rust/Cargo 1.95+, Windows MSVC, Electron/TypeScript, local fixture tests.

## 1. Establish the baseline

- [x] Inspect parent/submodule status and preserve untracked files.
- [x] Fetch and fast-forward parent `main` to `444c13365`; changes since the previous freeze are documentation only.
- [x] Resolve official latest non-prerelease through GitHub Releases API and the annotated tag. Confirm `3d86e43b` rather than `main` or the alpha branch tip.
- [x] Check out the existing submodule maintenance branch and fast-forward it to `origin/multi-ai/im-bridge` (`ac71f246`). Do not create a branch or rewrite history.

## 2. Integrate the release

- [x] Run `git merge --no-ff --no-commit 3d86e43b075adeba2193ec5c22ac92f55afd82e6` in the submodule.
- [x] Inspect every conflict with base/ours/theirs. Preserve structured IM, side-session automatic return, explicit dangerous bypass, disabled elevated helpers, terminal cursor policy and helper packaging.
- [x] Update the Rust workspace version and its workspace lock entries to `0.172.0+multi-ai.3d86e43b07`; preserve upstream dependency changes. Do not bump/release the Electron application as part of this source-only request.
- [x] Adapt only integration points changed upstream. In particular inspect Code Mode's new gRPC-only host path; the old V1 stdio smoke is not evidence for this release.
- [x] Read nested instructions before changing any file under a directory that contains them. Keep comments aligned with integration behavior.

## 3. Verify the actual integrated source

- [x] Scan unresolved conflict markers and inspect the diff against the upstream tag for unintended lost customizations.
- [x] Resolve pinned V8 dependencies using `python scripts/resolve-codex-v8-env.py x86_64-pc-windows-msvc`.
- [x] In MSVC environment, run `cargo check --locked --tests -p codex-cli -p codex-tui -p codex-code-mode-host` with bounded build jobs and local debug/incremental disabled. Investigate actual failures before changing code.
- [x] Run isolated native IM/sandbox/side-session regression selections with original 8 MiB stack; include CLI/config/helper tests. Preserve assertions and report warnings/skips.
- [x] Run parent `npm run typecheck`, `npm run test:remote-im -- --maxWorkers=2 --minWorkers=1`, and packaging helper tests if affected.
- [x] Build the integrated CLI/Code Mode host without producing installers. Check the real CLI version, fresh-home TUI login menu and a local Code Mode execution through the current protocol. Do not authenticate to a real account or send paid/model requests.

## 4. Record and integrate

- [x] Request an independent diff/evidence review and resolve concrete findings.
- [x] Record baseline, release URL, full commits, merge decisions, verification results and unverified boundaries in `docs/diagnostics/`.
- [ ] Commit and push the submodule maintenance history first, then commit/push the parent gitlink and this task's documentation on `main`. Exclude all pre-existing untracked content.
- [ ] Re-read remote refs and report exact commits and tests. Do not tag, publish a Release, run an installer or replace the installed application.

## Evidence-driven scope additions

- The new CLI daemon default broke the existing split host bundle. Explicit embedded launch arguments now preserve the product architecture and user-selected remote/service commands; 111 parent regressions passed.
- Actual 8 MiB tests exposed retained event-construction frames. Existing non-inline factories now cover the measured production entries and scenario, preserving stack budget and assertions.
- Current metadata updated 44 goldens; one additional native Windows path golden avoids post-wrap normalization.
- Final local validation: 208 native tests passed (7 leaky), 508 host IM passed (1 skipped), 111 launch/PTY passed, 26 packaging passed, full typecheck passed. Details are in the integration diagnostic.
