# Habit Monitoring and Managed Chrome Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the current “Skill 学习” workflow with a new “习惯监控” center that collects in-app behavior plus app-managed Chrome behavior, auto-enables low-risk flows and light UI personalization, and removes `模板` / `向导` from the top-level navigation.

**Architecture:** Extend the existing `electron/habit` pipeline instead of creating a parallel subsystem. Keep raw events in SQLite, add flow/session tables, add a managed Chrome session manager in the main process, evolve the existing aggregator/generator path into a flow engine, and replace the current `SkillStudioDialog` UI with a new `HabitMonitorDialog`. Topbar chrome changes stay in `src/App.tsx`; backend state stays behind preload IPC.

**Tech Stack:** Electron 33, React 18, TypeScript, `better-sqlite3`, Node `child_process.spawn`, Chrome DevTools Protocol over WebSocket, existing `electron/habit/*` pipeline, existing preload bridge, Vitest.

---

## File Structure

- Create: `electron/habit/managedChrome.ts`
  - Find Chrome, launch a managed session, connect/disconnect via CDP, and expose process/session state.
- Create: `electron/habit/managedChrome.test.ts`
  - Unit tests for Chrome path resolution, launch arguments, profile directory handling, and session cleanup.
- Create: `electron/habit/flowEngine.ts`
  - Convert aggregated events into `habit_flows`, classify risk, and generate low-risk app/site flows plus UI adjustments.
- Create: `electron/habit/flowEngine.test.ts`
  - Unit tests for low-risk vs high-risk classification, event-to-flow clustering, and default-enable decisions.
- Create: `electron/habit/managedChromeCollector.ts`
  - Normalize CDP events into stored `habit_events` rows without persisting sensitive text.
- Create: `electron/habit/managedChromeCollector.test.ts`
  - Tests for URL capture, click/input hint extraction, and privacy redaction.
- Create: `src/habit/HabitMonitorDialog.tsx`
  - New main dialog replacing `SkillStudioDialog`, with monitoring settings, managed Chrome session card, active flows, and candidate lists.
- Create: `src/habit/HabitMonitorDialog.test.tsx`
  - Interaction tests for settings toggles, managed Chrome controls, and active-flow enable/disable state.
- Create: `src/habit/ManagedChromePanel.tsx`
  - Focused panel/card for managed Chrome status and start/stop controls.
- Create: `src/habit/ManagedChromePanel.test.tsx`
  - Tests for session status rendering and button state transitions.
- Create: `src/habit/FlowsPanel.tsx`
  - Render active low-risk flows, UI adjustments, and high-risk candidates separately.
- Create: `src/habit/FlowsPanel.test.tsx`
  - Tests for grouping, enable/disable actions, and risk labeling.
- Create: `src/App.habitMonitor.test.tsx`
  - App-level tests for topbar entry changes, managed Chrome button behavior, and auto-personalized chrome visibility.
- Modify: `electron/store/db.ts`
  - Add schema for `habit_flows` and `managed_chrome_sessions`; extend `habit_events` semantics without destructive migration.
- Modify: `electron/habit/db.ts`
  - Add CRUD helpers for `habit_flows`, managed Chrome sessions, and richer habit event filtering.
- Modify: `electron/habit/db.test.ts`
  - Cover the new DB helpers and event/flow/session persistence.
- Modify: `electron/habit/settings.ts`
  - Expand habit settings to include managed Chrome collection, low-risk auto-enable, and UI personalization toggles.
- Modify: `electron/habit/settings.test.ts`
  - Validate new defaults and corrupted-file fallback.
- Modify: `electron/habit/collector.ts`
  - Support richer event payload/source metadata.
- Modify: `electron/habit/aggregator.ts`
  - Preserve useful clustering behavior while grouping the new site/app event kinds.
- Modify: `electron/habit/generator.ts`
  - Stop centering on prompt templates; delegate to the new flow engine and keep any legacy template path behind an explicit compatibility branch.
- Modify: `electron/habit/scheduler.ts`
  - Run the evolved aggregation/flow-generation pass and update `lastAggregatedAt`.
- Modify: `electron/habit/ipc.ts`
  - Expose new IPC for flows, managed Chrome controls, and richer settings.
- Modify: `electron/main.ts`
  - Register managed Chrome lifecycle, wire CDP collection, and ensure cleanup on app quit.
- Modify: `electron/preload.ts`
  - Add `window.api.habit.flows.*` and `window.api.habit.chrome.*`.
- Modify: `src/App.tsx`
  - Replace `Skill 学习` with `习惯监控`, add `托管 Chrome` topbar entry, remove `模板` / `向导` from primary chrome, and apply auto personalization.
- Modify: `src/components/GlobalSearchDialog.tsx`
  - Remove or demote `模板` / `向导` search entries; add `托管 Chrome` / `习惯监控`.
- Modify: `src/habit/FirstRunNoticeDialog.tsx`
  - Rewrite copy from “Skill 学习” to “习惯监控”.
- Modify: `src/habit/CollectionSettingsPanel.tsx`
  - Keep it as a narrow subsection for retention/raw-event controls inside the new dialog instead of a standalone primary view.
- Modify: `src/habit/habitTypes.ts`
  - Update types/labels from template-centric candidates to flow-centric rows.
- Modify: `src/styles.css`
  - Add styles for the new dialog/panels and remove obsolete topbar assumptions for `模板` / `向导`.

## Design Notes Locked Before Implementation

- [ ] Keep monitoring centered on two sources only: `app_ui` and `managed_chrome`.
- [ ] Only capture website behavior from Chrome instances launched by this app.
- [ ] Default-enable low-risk flows and light UI adjustments, but expose kill switches.
- [ ] Do not auto-enable site submits, login/logout, publish, delete, message send, payment, or any remote state mutation.
- [ ] Do not persist raw passwords, tokens, cookies, payment info, or sensitive input text.
- [ ] Remove `模板` and `向导` from top-level navigation in v1; do not block on preserving them as first-class UI.
- [ ] Reuse the existing `electron/habit` pipeline instead of building a second habit subsystem.

## Task 1: Expand the Habit Data Model, Settings, and Persistence Layer

**Files:**
- Modify: `electron/store/db.ts`
- Modify: `electron/habit/db.ts`
- Modify: `electron/habit/db.test.ts`
- Modify: `electron/habit/settings.ts`
- Modify: `electron/habit/settings.test.ts`
- Modify: `src/habit/habitTypes.ts`

- [ ] **Step 1: Write the failing tests for the new settings defaults**

Add tests in `electron/habit/settings.test.ts` for these new defaults:

```ts
expect(settings.enabled).toBe(true)
expect(settings.collectManagedChrome).toBe(true)
expect(settings.autoEnableLowRiskFlows).toBe(true)
expect(settings.autoPersonalizeUi).toBe(true)
```

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/habit/settings.test.ts
```

Expected: FAIL because the new fields do not exist yet.

- [ ] **Step 2: Extend the habit settings shape**

Update `electron/habit/settings.ts` to add:

```ts
export interface HabitSettings {
  enabled: boolean
  kinds: Partial<Record<HabitEventKind, boolean>>
  retentionDays: number
  firstRunNoticeShownAt: number
  lastAggregatedAt: number
  collectManagedChrome: boolean
  autoEnableLowRiskFlows: boolean
  autoPersonalizeUi: boolean
}
```

Update `DEFAULT_HABIT_SETTINGS` and `mergeWithDefaults()` accordingly.

- [ ] **Step 3: Write the failing DB tests for flows and Chrome sessions**

Add tests in `electron/habit/db.test.ts` for:
- inserting/listing `habit_flows`
- updating a flow status from `active` to `disabled`
- inserting/listing `managed_chrome_sessions`
- filtering `habit_events` by `source`

Use expected row shapes like:

```ts
expect(flow.kind).toBe('site-flow')
expect(flow.risk_level).toBe('low')
expect(session.running).toBe(1)
```

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/habit/db.test.ts
```

Expected: FAIL because the schema/helpers do not exist yet.

- [ ] **Step 4: Add the new SQLite schema**

Extend `electron/store/db.ts` with:

```sql
CREATE TABLE IF NOT EXISTS habit_flows (
  id                 INTEGER PRIMARY KEY AUTOINCREMENT,
  kind               TEXT NOT NULL,
  title              TEXT NOT NULL,
  summary            TEXT NOT NULL,
  evidence_count     INTEGER NOT NULL,
  risk_level         TEXT NOT NULL,
  enabled_by_default INTEGER NOT NULL DEFAULT 0,
  status             TEXT NOT NULL DEFAULT 'candidate',
  payload            TEXT NOT NULL,
  created_at         INTEGER NOT NULL,
  updated_at         INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS managed_chrome_sessions (
  id              INTEGER PRIMARY KEY AUTOINCREMENT,
  port            INTEGER NOT NULL,
  profile_dir     TEXT NOT NULL,
  started_at      INTEGER NOT NULL,
  last_active_at  INTEGER NOT NULL,
  running         INTEGER NOT NULL DEFAULT 1,
  last_active_url TEXT
);
```

Keep the existing `habit_events` table intact; use payload/source conventions and helper-level parsing instead of destructive migration.

- [ ] **Step 5: Implement the new DB helpers**

Add helpers in `electron/habit/db.ts` such as:

```ts
export function insertHabitFlow(input: NewHabitFlow): HabitFlowRow
export function listHabitFlows(opts?: { statuses?: HabitFlowStatus[] }): HabitFlowRow[]
export function updateHabitFlowStatus(id: number, status: HabitFlowStatus): void
export function upsertManagedChromeSession(input: ManagedChromeSessionInput): ManagedChromeSessionRow
export function listManagedChromeSessions(limit = 20): ManagedChromeSessionRow[]
```

Keep row types serialized through JSON for `payload`.

- [ ] **Step 6: Update renderer-visible habit types**

Expand `src/habit/habitTypes.ts` with:

```ts
export type HabitEventKind =
  | 'pty_cmd'
  | 'ai_prompt_main'
  | 'ai_prompt_repo'
  | 'diff_annotation'
  | 'repo_view_annotation'
  | 'template_used'
  | 'plan_imported'
  | 'panel_open'
  | 'action_triggered'
  | 'site_visit'
  | 'site_click'
  | 'site_input_hint'
  | 'tab_switch'

export type HabitFlowKind = 'app-flow' | 'site-flow' | 'ui-adjustment'
export type HabitFlowRisk = 'low' | 'high'
export type HabitFlowStatus = 'candidate' | 'active' | 'disabled'
```

- [ ] **Step 7: Re-run focused tests and typecheck**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/habit/settings.test.ts electron/habit/db.test.ts
npm.cmd run typecheck
```

Expected: PASS

- [ ] **Step 8: Commit the data-layer changes**

```bash
git add electron/store/db.ts electron/habit/db.ts electron/habit/db.test.ts electron/habit/settings.ts electron/habit/settings.test.ts src/habit/habitTypes.ts
git commit -m "feat: extend habit monitoring data model"
```

## Task 2: Add the Managed Chrome Backend and Safe CDP Event Collection

**Files:**
- Create: `electron/habit/managedChrome.ts`
- Create: `electron/habit/managedChrome.test.ts`
- Create: `electron/habit/managedChromeCollector.ts`
- Create: `electron/habit/managedChromeCollector.test.ts`
- Modify: `electron/main.ts`
- Modify: `electron/habit/ipc.ts`
- Modify: `electron/preload.ts`
- Modify: `electron/habit/collector.ts`

- [ ] **Step 1: Write the failing managed Chrome launch tests**

Add tests in `electron/habit/managedChrome.test.ts` covering:
- Chrome executable resolution from common Windows paths
- launch args include `--remote-debugging-port` and `--user-data-dir`
- stopping the manager kills the child process and clears state

Use assertions like:

```ts
expect(args).toContain('--remote-debugging-port=9222')
expect(args.some((a) => a.startsWith('--user-data-dir='))).toBe(true)
```

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/habit/managedChrome.test.ts
```

Expected: FAIL because the module does not exist.

- [ ] **Step 2: Implement the managed Chrome session manager**

Create `electron/habit/managedChrome.ts` with a singleton-like class:

```ts
export interface ManagedChromeState {
  running: boolean
  port: number | null
  profileDir: string | null
  pid: number | null
  lastActiveUrl: string | null
}

export class ManagedChromeManager {
  start(): Promise<ManagedChromeState>
  stop(): Promise<void>
  getState(): ManagedChromeState
  focus(): Promise<void>
}
```

Launch via `child_process.spawn` and arguments:

```ts
[
  `--remote-debugging-port=${port}`,
  `--user-data-dir=${profileDir}`,
  '--no-first-run',
  '--no-default-browser-check'
]
```

- [ ] **Step 3: Write the failing collector privacy tests**

Add tests in `electron/habit/managedChromeCollector.test.ts` to ensure:
- URL visits become `site_visit`
- click targets become `site_click`
- text inputs only store hints, not full values
- password-like inputs are dropped entirely

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/habit/managedChromeCollector.test.ts
```

Expected: FAIL because the collector does not exist.

- [ ] **Step 4: Implement CDP-to-event normalization**

Create `electron/habit/managedChromeCollector.ts` with functions like:

```ts
export function buildSiteVisitEvent(url: string, tabId: string): RecordHabitEventInput
export function buildSiteClickEvent(input: ClickSignal): RecordHabitEventInput
export function buildSiteInputHintEvent(input: InputSignal): RecordHabitEventInput | null
```

Privacy rule:

```ts
if (input.type === 'password') return null
const text = truncate(input.label ?? input.role ?? input.placeholder ?? 'input')
```

- [ ] **Step 5: Add IPC and preload wiring**

In `electron/habit/ipc.ts` and `electron/preload.ts`, add:

```ts
habit: {
  chrome: {
    getState(): Promise<ManagedChromeState>
    start(): Promise<{ ok: boolean; value?: ManagedChromeState; error?: string }>
    stop(): Promise<{ ok: boolean; error?: string }>
    focus(): Promise<{ ok: boolean; error?: string }>
  }
}
```

In `electron/main.ts`, own the manager lifecycle and ensure cleanup on app quit.

- [ ] **Step 6: Re-run focused tests and typecheck**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/habit/managedChrome.test.ts electron/habit/managedChromeCollector.test.ts
npm.cmd run typecheck
```

Expected: PASS

- [ ] **Step 7: Commit the managed Chrome backend**

```bash
git add electron/habit/managedChrome.ts electron/habit/managedChrome.test.ts electron/habit/managedChromeCollector.ts electron/habit/managedChromeCollector.test.ts electron/main.ts electron/habit/ipc.ts electron/preload.ts electron/habit/collector.ts
git commit -m "feat: add managed chrome monitoring backend"
```

## Task 3: Evolve the Existing Habit Pipeline into a Flow Engine

**Files:**
- Create: `electron/habit/flowEngine.ts`
- Create: `electron/habit/flowEngine.test.ts`
- Modify: `electron/habit/aggregator.ts`
- Modify: `electron/habit/generator.ts`
- Modify: `electron/habit/scheduler.ts`
- Modify: `electron/habit/generatorRegistry.ts`
- Modify: `electron/habit/ipc.ts`

- [ ] **Step 1: Write the failing flow-engine tests**

Add tests in `electron/habit/flowEngine.test.ts` for:
- repeated `site_visit` samples become a `site-flow`
- repeated `panel_open` samples become an `app-flow`
- “hide template / hide wizard” becomes a low-risk `ui-adjustment`
- click/submit patterns marked as mutating become `high`

Use explicit expectations like:

```ts
expect(flow.kind).toBe('site-flow')
expect(flow.risk_level).toBe('low')
expect(flow.status).toBe('active')
```

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/habit/flowEngine.test.ts
```

Expected: FAIL because the module does not exist.

- [ ] **Step 2: Implement the flow engine**

Create `electron/habit/flowEngine.ts` with:

```ts
export interface GeneratedFlow {
  kind: HabitFlowKind
  title: string
  summary: string
  riskLevel: HabitFlowRisk
  enabledByDefault: boolean
  payload: Record<string, unknown>
}

export function generateFlowsFromClusters(clusters: AggregatedCluster[]): GeneratedFlow[] { ... }
```

Rule examples:

```ts
if (cluster.kind === 'site_visit') {
  return {
    kind: 'site-flow',
    title: `打开 ${originHost}`,
    summary: `经常访问 ${normalizedPath}`,
    riskLevel: 'low',
    enabledByDefault: true,
    payload: { action: 'open-managed-chrome-url', url: stableUrl }
  }
}
```

- [ ] **Step 3: Expand clustering for new event kinds**

Update `electron/habit/aggregator.ts` so the new site/app kinds cluster sensibly:
- `site_visit` should normalize query noise and focus on origin/path patterns
- `site_click` / `site_input_hint` should normalize element hints
- `panel_open` should normalize panel ids

Do not delete existing clustering support for `template_used` yet; keep it backward-compatible.

- [ ] **Step 4: Rewire scheduler/generator to produce `habit_flows`**

Change the scheduler path from “generate prompt template candidates” to “generate/update habit flows”.

Keep legacy template generation only as an optional compatibility path, not the default center of the pipeline.

At minimum:

```ts
const clusters = aggregateHabitEvents(rows)
const flows = generateFlowsFromClusters(clusters)
persistGeneratedFlows(flows)
```

- [ ] **Step 5: Add flow-list IPC**

Expose in `electron/habit/ipc.ts` and `preload.ts`:

```ts
habit: {
  flows: {
    list(): Promise<HabitFlowRow[]>
    updateStatus(req: { id: number; status: HabitFlowStatus }): Promise<{ ok: boolean }>
  }
}
```

- [ ] **Step 6: Re-run focused tests and typecheck**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/habit/flowEngine.test.ts electron/habit/aggregator.test.ts
npm.cmd run typecheck
```

Expected: PASS

- [ ] **Step 7: Commit the flow engine**

```bash
git add electron/habit/flowEngine.ts electron/habit/flowEngine.test.ts electron/habit/aggregator.ts electron/habit/generator.ts electron/habit/scheduler.ts electron/habit/generatorRegistry.ts electron/habit/ipc.ts electron/preload.ts
git commit -m "feat: generate habit flows from monitoring data"
```

## Task 4: Replace the Skill UI with the Habit Monitor UI

**Files:**
- Create: `src/habit/HabitMonitorDialog.tsx`
- Create: `src/habit/HabitMonitorDialog.test.tsx`
- Create: `src/habit/ManagedChromePanel.tsx`
- Create: `src/habit/ManagedChromePanel.test.tsx`
- Create: `src/habit/FlowsPanel.tsx`
- Create: `src/habit/FlowsPanel.test.tsx`
- Modify: `src/habit/FirstRunNoticeDialog.tsx`
- Modify: `src/habit/CollectionSettingsPanel.tsx`
- Modify: `src/styles.css`

- [ ] **Step 1: Write the failing dialog tests**

Add tests in `src/habit/HabitMonitorDialog.test.tsx` for:
- loading settings and managed Chrome state on open
- toggling `collectManagedChrome`
- rendering active low-risk flows separately from high-risk candidates
- disabling a flow from the active list

Run:

```bash
node .\node_modules\vitest\vitest.mjs run src/habit/HabitMonitorDialog.test.tsx src/habit/ManagedChromePanel.test.tsx src/habit/FlowsPanel.test.tsx
```

Expected: FAIL because the new components do not exist.

- [ ] **Step 2: Implement `ManagedChromePanel.tsx`**

Render:

```tsx
<section className="habit-managed-chrome-card">
  <h4>托管 Chrome</h4>
  <p>{state.running ? `运行中 · 端口 ${state.port}` : '未启动'}</p>
  <button onClick={onStart}>启动</button>
  <button onClick={onFocus} disabled={!state.running}>聚焦</button>
  <button onClick={onStop} disabled={!state.running}>停止</button>
</section>
```

- [ ] **Step 3: Implement `FlowsPanel.tsx`**

Render three buckets:
- active low-risk flows
- active UI adjustments
- high-risk candidates

Per-row actions:

```tsx
<button onClick={() => onDisable(flow.id)}>关闭</button>
```

For high-risk candidates, show a non-interactive badge like `需人工确认`.

- [ ] **Step 4: Implement `HabitMonitorDialog.tsx`**

This dialog should replace `SkillStudioDialog` and compose:
- a settings section
- `ManagedChromePanel`
- `FlowsPanel`
- optional recent raw events section retained from the old collection view

Suggested tab structure:

```ts
type Tab = 'overview' | 'collection'
```

Keep the collection/retention settings visible, but do not center the UX on prompt-template candidates anymore.

- [ ] **Step 5: Rewrite the first-run notice copy**

Update `src/habit/FirstRunNoticeDialog.tsx` so the copy says “习惯监控” and explains:
- in-app habit collection
- managed Chrome collection
- low-risk flows auto-enable by default
- settings can disable collection or auto behavior

- [ ] **Step 6: Add styles and run the UI tests**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run src/habit/HabitMonitorDialog.test.tsx src/habit/ManagedChromePanel.test.tsx src/habit/FlowsPanel.test.tsx
npm.cmd run typecheck
```

Expected: PASS

- [ ] **Step 7: Commit the new habit monitor UI**

```bash
git add src/habit/HabitMonitorDialog.tsx src/habit/HabitMonitorDialog.test.tsx src/habit/ManagedChromePanel.tsx src/habit/ManagedChromePanel.test.tsx src/habit/FlowsPanel.tsx src/habit/FlowsPanel.test.tsx src/habit/FirstRunNoticeDialog.tsx src/habit/CollectionSettingsPanel.tsx src/styles.css
git commit -m "feat: replace skill studio with habit monitor UI"
```

## Task 5: Integrate Topbar Changes, Auto Personalization, and App Wiring

**Files:**
- Create: `src/App.habitMonitor.test.tsx`
- Modify: `src/App.tsx`
- Modify: `src/components/GlobalSearchDialog.tsx`
- Modify: `src/styles.css`

- [ ] **Step 1: Write the failing app integration tests**

Add tests in `src/App.habitMonitor.test.tsx` to verify:
- topbar shows `习惯监控` instead of `Skill 学习`
- topbar shows `托管 Chrome`
- `模板` and `向导` are absent from primary navigation
- active UI-adjustment flows can hide/demote low-frequency entries

Run:

```bash
node .\node_modules\vitest\vitest.mjs run src/App.habitMonitor.test.tsx
```

Expected: FAIL because the topbar still exposes the old entries and there is no managed Chrome action yet.

- [ ] **Step 2: Swap the primary topbar actions in `src/App.tsx`**

Replace:

```tsx
📋 模板
🎓 Skill 学习
❓ 向导
```

with:

```tsx
🧠 习惯监控
🌐 托管 Chrome
```

Keep the old dialogs/components out of the primary topbar flow. Do not delete the files yet; just stop surfacing them as primary actions.

- [ ] **Step 3: Apply low-risk UI adjustments at render time**

In `src/App.tsx`, load active `ui-adjustment` flows and derive simple UI flags, for example:

```ts
const hideTemplatesEntry = autoPersonalizeUi && activeUiAdjustments.some((f) => f.payload.action === 'hide-templates-entry')
const hideWizardEntry = autoPersonalizeUi && activeUiAdjustments.some((f) => f.payload.action === 'hide-wizard-entry')
```

The v1 shortcut is acceptable:
- if `autoPersonalizeUi` is on, default to hiding `模板` and `向导`
- represent those as active low-risk UI adjustments in the flow list

- [ ] **Step 4: Add managed Chrome button behavior**

The topbar `托管 Chrome` action should:
- start the managed session when none exists
- focus the existing session when already running

This must call the new preload APIs instead of shelling out directly from the renderer.

- [ ] **Step 5: Update global search and dead-end UI**

In `src/components/GlobalSearchDialog.tsx`:
- remove or demote the `模板` and `向导` hits
- add hits for `习惯监控` and `托管 Chrome`

In `src/App.tsx`:
- switch all remaining `SkillStudioDialog` usage sites to `HabitMonitorDialog`
- do not leave both dialogs active from the main app

- [ ] **Step 6: Re-run focused UI tests and typecheck**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run src/habit/HabitMonitorDialog.test.tsx src/habit/ManagedChromePanel.test.tsx src/habit/FlowsPanel.test.tsx
npm.cmd run typecheck
```

Expected: PASS

- [ ] **Step 7: Commit the app integration**

```bash
git add src/App.tsx src/App.habitMonitor.test.tsx src/components/GlobalSearchDialog.tsx src/styles.css
git commit -m "feat: wire habit monitor and managed chrome into app chrome"
```

## Task 6: End-to-End Verification and Safety Checks

**Files:**
- Modify: none

- [ ] **Step 1: Run the focused automated suite**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/habit/settings.test.ts electron/habit/db.test.ts electron/habit/managedChrome.test.ts electron/habit/managedChromeCollector.test.ts electron/habit/flowEngine.test.ts src/habit/HabitMonitorDialog.test.tsx src/habit/ManagedChromePanel.test.tsx src/habit/FlowsPanel.test.tsx
```

Expected: PASS

- [ ] **Step 2: Run full typecheck**

Run:

```bash
npm.cmd run typecheck
```

Expected: PASS

- [ ] **Step 3: Manual verify the managed Chrome lifecycle**

Use the app UI to:
- click topbar `托管 Chrome`
- confirm a dedicated Chrome instance starts
- confirm `习惯监控` shows it as running
- click stop and confirm it exits cleanly

Expected:
- no crash
- session state updates in UI
- profile directory is app-managed, not the user’s default Chrome profile

- [ ] **Step 4: Manual verify website habit collection privacy**

Inside the managed Chrome session:
- visit a normal site
- click a few navigation links
- type into a search field

Expected:
- `site_visit` / `site_click` / `site_input_hint` events appear
- search text is not stored verbatim as full raw input
- password fields do not produce stored input events

- [ ] **Step 5: Manual verify low-risk auto-enable and UI personalization**

Expected after enough sample data or seeded flows:
- low-risk site/app flows move into the active list automatically
- `模板` and `向导` are no longer primary topbar actions
- turning off `自动个性化界面` restores the related UI decisions

- [ ] **Step 6: Final working-tree check**

Run:

```bash
git status --short
```

Expected: clean working tree after all task commits.

## Self-Review Checklist

- Spec coverage:
  - `Skill 学习` 重构为 `习惯监控` is covered in Tasks 4 and 5
  - managed Chrome launch points are covered in Tasks 2, 4, and 5
  - dual-source collection is covered in Tasks 1, 2, and 3
  - low-risk default-enable and UI personalization are covered in Tasks 3 and 5
  - `模板` / `向导` demotion/removal is covered in Task 5
- Placeholder scan:
  - no unresolved placeholder wording remains in task steps
  - every task names exact files and exact test commands
- Type consistency:
  - settings names align across backend and renderer
  - flow statuses use `candidate | active | disabled`
  - monitoring sources use `app_ui | managed_chrome`
