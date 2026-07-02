# README Rewrite Design

## Background

The current README describes many implemented features, but it reads like an internal implementation inventory. It is also weak on newer product surfaces such as Skill management, Skill orchestration, habit collection, the current iOS Remote IM flow, and the practical local-first positioning.

## Goal

Rewrite `README.md` from scratch so the GitHub landing page presents the current product clearly:

- Lead with what Multi-AI Code is and why it exists.
- Explain the major user-facing workflows before internal architecture.
- Keep developer setup, build, test, and platform constraints easy to find.
- Remove outdated wording about older workflow models.

## Non-Goals

- Do not change source code or product behavior.
- Do not add screenshots or regenerate visual assets.
- Do not document speculative features that are not present in the current codebase.
- Do not preserve the old README section order.

## Audience

The README should serve two audiences:

- A GitHub visitor deciding whether the project is relevant.
- A developer who wants to run, test, package, or install the app locally.

## Proposed Structure

1. `Multi-AI Code` title and short positioning.
2. `What It Is` with a concise product description.
3. `Why Use It` with scenario-led bullets.
4. `Core Workflows` grouped by real user workflows:
   - AI CLI workspace
   - Tasks and scheduled tasks
   - Code review and repo viewer
   - Build, runtime, and logs
   - Skills, Skill pipelines, and habit collection
   - Remote IM, iOS client, and voice transcription
5. `How It Works` with a compact local-first architecture summary.
6. `Data Ownership` with the most important storage locations and privacy boundaries.
7. `Quick Start` for desktop development.
8. `iOS Remote IM` for simulator and device build commands.
9. `Packaging` for desktop builds and ASR resources.
10. `Test Commands`.
11. `Tech Stack`.
12. `Current Limits`.

## Content Rules

- Prefer product language over implementation inventory.
- Keep commands exact and copyable.
- Keep tables short.
- Keep old details only when they help a developer run or trust the app.
- Be explicit that the app uses real local `claude` / `codex` CLIs and does not embed a model.
- Be explicit that Remote IM currently uses built-in test credentials and is not production App Store configuration.

## Acceptance Criteria

- `README.md` is substantially rewritten, not lightly edited.
- The first screen explains the product without requiring architecture context.
- Skill management, Skill orchestration, habit collection, iOS Remote IM, and local Whisper ASR are represented.
- Old four-stage workflow language is removed.
- Existing run, build, iOS, and test commands remain available.
- No source files outside README and planning/spec docs are changed.
