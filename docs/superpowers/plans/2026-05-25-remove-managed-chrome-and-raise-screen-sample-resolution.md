# Remove Managed Chrome And Raise Screen Sample Resolution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove managed Chrome completely and make background screen samples capture near the primary display's real pixel size so text remains readable.

**Architecture:** Collapse habit monitoring back to app activity plus screen sampler only. Delete the managed Chrome branch end-to-end, then raise screen sampler capture size at the service boundary where Electron desktop capture is requested.

**Tech Stack:** Electron, React, TypeScript, Vitest, better-sqlite3

---

### Task 1: Lock the regressions with tests

**Files:**
- Modify: `src/App.habitMonitor.test.tsx`
- Modify: `src/habit/HabitMonitorDialog.test.tsx`
- Create: `electron/habit/screenSamplerService.test.ts`

- [ ] Add renderer assertions that the app no longer renders or advertises managed Chrome.
- [ ] Add a sampler-service unit test that expects capture requests to use the primary display's physical pixel size.
- [ ] Run the focused tests and confirm they fail for the expected reasons before implementation.

### Task 2: Remove managed Chrome runtime and UI

**Files:**
- Modify: `electron/main.ts`
- Modify: `electron/preload.ts`
- Modify: `electron/habit/ipc.ts`
- Modify: `electron/habit/db.ts`
- Modify: `electron/habit/settings.ts`
- Modify: `electron/store/db.ts`
- Modify: `src/App.tsx`
- Modify: `src/habit/HabitMonitorDialog.tsx`
- Modify: `src/habit/FirstRunNoticeDialog.tsx`
- Modify: `src/habit/habitTypes.ts`
- Delete: `electron/habit/managedChrome.ts`
- Delete: `electron/habit/managedChrome.test.ts`
- Delete: `electron/habit/managedChromeCollector.ts`
- Delete: `electron/habit/managedChromeCollector.test.ts`
- Delete: `src/habit/ManagedChromePanel.tsx`
- Delete: `src/habit/ManagedChromePanel.test.tsx`

- [ ] Remove managed Chrome creation, shutdown, IPC handlers, preload types, and renderer calls.
- [ ] Remove managed Chrome DB helpers, table creation, settings fields, and tests that depended on them.
- [ ] Remove topbar buttons, search entries, and habit-monitor sections tied to managed Chrome.

### Task 3: Raise background screen-sample fidelity

**Files:**
- Modify: `electron/habit/screenSamplerService.ts`
- Modify: `electron/habit/screenSampler.ts`
- Modify: `electron/habit/screenSampler.test.ts`
- Create: `electron/habit/screenSamplerService.test.ts`

- [ ] Implement a helper that resolves desktop-capture dimensions from the primary display's physical size.
- [ ] Use that helper in the sampler service so capture requests are no longer capped at 1280x720.
- [ ] Keep saved frame metadata and file layout stable while updating any affected tests.

### Task 4: Verify and clean up

**Files:**
- Modify: `docs/superpowers/specs/2026-05-25-remove-managed-chrome-and-raise-screen-sample-resolution-design.md`
- Modify: `docs/superpowers/plans/2026-05-25-remove-managed-chrome-and-raise-screen-sample-resolution.md`

- [ ] Run the focused Vitest suites for habit monitor, screen sampler, and app chrome.
- [ ] Run `tsc --noEmit -p tsconfig.node.json` and `tsc --noEmit -p tsconfig.web.json`.
- [ ] Review the final diff to make sure no managed Chrome references remain in runtime code.
