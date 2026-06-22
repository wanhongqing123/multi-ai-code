# Settings Center Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert the current settings dialog into the approved settings-center layout.

**Architecture:** Keep the existing React state and save flow in `AiSettingsDialog.tsx`. Change only JSX structure and CSS classes, while reusing the existing screenshot, build, and runtime configuration logic.

**Tech Stack:** React, TypeScript, Vitest server-side rendering tests, existing CSS variables in `src/styles.css`.

---

### Task 1: Structural Test

**Files:**
- Modify: `src/components/AiSettingsDialog.test.tsx`

- [ ] Add an assertion that rendered markup contains `ai-settings-shell`, `ai-settings-sidebar`, `ai-settings-content`, `ai-settings-footer`, and `ai-settings-hero-card`.
- [ ] Run `npx vitest run src/components/AiSettingsDialog.test.tsx` and verify it fails before implementation.

### Task 2: Settings Center Markup

**Files:**
- Modify: `src/components/AiSettingsDialog.tsx`

- [ ] Replace the narrow modal body with a header, sidebar/content shell, and footer.
- [ ] Keep `handleSave`, all state, and all persistence calls unchanged.
- [ ] Update `SettingsSection` and `ScreenshotSettingsSection` class names to match the new layout.

### Task 3: Settings Center Styles

**Files:**
- Modify: `src/styles.css`

- [ ] Replace the old `.ai-settings-*` layout rules with wider modal, sidebar, content grid, hero card, and sticky footer styles.
- [ ] Preserve existing build/runtime nested form styles.
- [ ] Add responsive fallback so the sidebar stacks on narrow windows.

### Task 4: Verification

**Commands:**
- `npx vitest run src/components/AiSettingsDialog.test.tsx src/components/ProjectBuildSettingsSection.test.tsx src/components/ProjectRuntimeSettingsSection.test.tsx`
- `npm run typecheck`

- [ ] Fix only issues caused by this redesign.
