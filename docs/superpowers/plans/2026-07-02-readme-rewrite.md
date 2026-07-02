# README Rewrite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the outdated `README.md` with a current, product-first project introduction for Multi-AI Code.

**Architecture:** This is a documentation-only change. The README should describe the current Electron desktop app, local AI CLI workflow, Skill system, Remote IM/iOS companion flow, ASR packaging, and developer commands without changing application code.

**Tech Stack:** Markdown, existing npm scripts, existing iOS/Xcode commands.

---

## File Structure

- Modify `README.md`
  - Replace the old feature-inventory document with a new product-first README.
  - Keep copyable development, packaging, iOS, and test commands.
- Do not modify runtime source files.

## Task 1: Rewrite README

**Files:**
- Modify: `README.md`

- [x] **Step 1: Replace the document structure**

Use the structure approved in `docs/superpowers/specs/2026-07-02-readme-rewrite-design.md`:

1. Title and short positioning.
2. What It Is.
3. Why Use It.
4. Core Workflows.
5. How It Works.
6. Data Ownership.
7. Quick Start.
8. iOS Remote IM.
9. Packaging.
10. Test Commands.
11. Tech Stack.
12. Current Limits.

- [x] **Step 2: Refresh product content**

Cover the current product surfaces:

- Local `claude` / `codex` CLI workspace.
- Normal tasks, scheduled tasks, code review, repository viewer, project build, runtime logs.
- Skill management, Skill orchestration, prompt templates, and habit collection.
- Remote IM desktop bridge, iOS client, voice messages, and local Whisper ASR.
- Local-first data ownership and configuration boundaries.

- [x] **Step 3: Preserve useful commands**

Keep commands for:

- Desktop install and development.
- Desktop packaging.
- ASR resource preparation.
- iOS simulator tests/builds.
- iOS device build/install guidance.
- Repository test commands.

## Task 2: Verify Documentation Quality

**Files:**
- Inspect: `README.md`

- [x] **Step 1: Check Markdown whitespace**

Run:

```bash
git diff --check README.md
```

Expected: no whitespace errors.

- [x] **Step 2: Check old/stale wording is gone**

Run:

```bash
rg -n "四阶段|主奴|主人|奴隶|持续交付集成管理平台" README.md
```

Expected: no matches.

- [x] **Step 3: Review the final diff**

Run:

```bash
git diff -- README.md docs/superpowers/plans/2026-07-02-readme-rewrite.md
```

Expected: only the README rewrite and implementation plan are changed.
