# Scheduled Task Manager Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a project-scoped scheduled task manager that lets users create AI tasks, preview the prompt, persist schedules, and send due tasks to the current AICLI session through a serialized queue.

**Architecture:** Add a focused Electron-side task store, prompt builder, and scheduler queue under `electron/scheduledTasks`. Expose IPC through preload as `window.api.scheduledTasks`, then add a React dialog under `src/scheduled-tasks` and a topbar entry in `src/App.tsx`. The first version uses the current AICLI only and queues work when no send target is available.

**Tech Stack:** Electron IPC, better-sqlite3, React, TypeScript, Vitest, CSS modules via global `src/styles.css`.

---

### Task 1: Data model and prompt builder

**Files:**
- Modify: `electron/store/db.ts`
- Create: `electron/scheduledTasks/types.ts`
- Create: `electron/scheduledTasks/taskStore.ts`
- Create: `electron/scheduledTasks/promptBuilder.ts`
- Test: `electron/scheduledTasks/taskStore.test.ts`
- Test: `electron/scheduledTasks/promptBuilder.test.ts`

- [ ] **Step 1: Add database tables**

Add `scheduled_tasks` and `scheduled_task_runs` to `SCHEMA` in `electron/store/db.ts`, plus indexes for `project_id`, `enabled`, `next_run_at`, and `status`.

- [ ] **Step 2: Define shared task types**

Create `electron/scheduledTasks/types.ts` with:

```ts
export type ScheduledTaskScheduleType = 'once' | 'daily' | 'weekly'
export type ScheduledTaskRunStatus =
  | 'pending'
  | 'queued'
  | 'running'
  | 'succeeded'
  | 'failed'
  | 'timed_out'
  | 'cancelled'
  | 'skipped'

export interface ScheduledTask {
  id: number
  projectId: string
  name: string
  description: string
  goal: string
  instructions: string[]
  enabled: boolean
  scheduleType: ScheduledTaskScheduleType
  scheduleTime: string
  scheduleDays: number[]
  nextRunAt: number | null
  timeoutMinutes: number
  allowCodeChanges: boolean
  allowGitCommit: boolean
  requireTestConfirmation: boolean
  createdAt: number
  updatedAt: number
  lastRun: ScheduledTaskRun | null
}

export interface ScheduledTaskRun {
  id: number
  taskId: number
  status: ScheduledTaskRunStatus
  scheduledAt: number
  startedAt: number | null
  finishedAt: number | null
  prompt: string
  outputExcerpt: string | null
  error: string | null
  timeoutMinutes: number
}
```

- [ ] **Step 3: Implement CRUD store**

Create `taskStore.ts` with `listScheduledTasks`, `createScheduledTask`, `updateScheduledTask`, `deleteScheduledTask`, `setScheduledTaskEnabled`, `createScheduledTaskRun`, `updateScheduledTaskRun`, `listDueScheduledTasks`, and `computeNextRunAt`.

- [ ] **Step 4: Implement prompt builder**

Create `buildScheduledTaskPrompt(task, context)` that emits a Chinese prompt beginning with `你现在要执行一个由 Multi-AI Code 触发的定时任务。` and includes task name, working directory, goal, requirements, safety rules, and output rules.

- [ ] **Step 5: Add tests**

Add tests for CRUD roundtrip, next-run calculation, due-task listing, and prompt text containing the safety constraints.

### Task 2: Scheduler runtime and IPC

**Files:**
- Create: `electron/scheduledTasks/scheduler.ts`
- Create: `electron/scheduledTasks/ipc.ts`
- Modify: `electron/main.ts`
- Modify: `electron/preload.ts`
- Test: `electron/scheduledTasks/scheduler.test.ts`

- [ ] **Step 1: Implement scheduler service**

Create a polling scheduler with `startScheduledTaskScheduler`, `stopScheduledTaskScheduler`, `getScheduledTaskQueueState`, `setScheduledTaskSendHandler`, and `runScheduledTaskScanOnce`.

- [ ] **Step 2: Keep execution serialized**

Use an in-memory `running` flag and FIFO queue. A due task creates a run record, then waits until a send handler reports the current AICLI is available.

- [ ] **Step 3: Add IPC handlers**

Register handlers:

```text
scheduled-tasks:list
scheduled-tasks:create
scheduled-tasks:update
scheduled-tasks:delete
scheduled-tasks:set-enabled
scheduled-tasks:run-now
scheduled-tasks:queue-state
```

- [ ] **Step 4: Wire main process**

Call `registerScheduledTaskIpc()` and `startScheduledTaskScheduler()` from `electron/main.ts`.

- [ ] **Step 5: Expose preload API**

Expose `window.api.scheduledTasks` with typed methods mirroring IPC.

- [ ] **Step 6: Add scheduler tests**

Verify due tasks enqueue in order, unavailable AICLI keeps runs queued, and successful send marks runs as running or succeeded according to the first-version behavior.

### Task 3: React UI

**Files:**
- Create: `src/scheduled-tasks/ScheduledTaskDialog.tsx`
- Create: `src/scheduled-tasks/ScheduledTaskEditorDialog.tsx`
- Create: `src/scheduled-tasks/scheduledTaskViewModel.ts`
- Create: `src/scheduled-tasks/ScheduledTaskDialog.test.tsx`
- Create: `src/scheduled-tasks/ScheduledTaskEditorDialog.test.tsx`
- Modify: `src/App.tsx`
- Modify: `src/App.habitMonitor.test.tsx`
- Modify: `src/styles.css`

- [ ] **Step 1: Add topbar state and button**

In `src/App.tsx`, add `showScheduledTaskDialog`, render `⏰ 定时任务`, and mount `ScheduledTaskDialog` when a project is selected.

- [ ] **Step 2: Build manager dialog**

Render summary cards, toolbar, search, task list, selected task detail, run button, edit button, and current AICLI status.

- [ ] **Step 3: Build editor dialog**

Render the create/edit form with task name, description, goal, common instruction checkboxes, schedule fields, timeout, safety options, and live prompt preview.

- [ ] **Step 4: Add view-model helpers**

Add helpers for formatting status, dates, schedule labels, and default task draft values.

- [ ] **Step 5: Add CSS**

Append `.scheduled-task-*` styles in `src/styles.css`, reusing `drawer-btn`, `modal`, and project color variables.

- [ ] **Step 6: Add component tests**

Verify manager labels, empty state, selected details, editor fields, default safety settings, and prompt preview.

### Task 4: AICLI send integration

**Files:**
- Modify: `src/App.tsx`
- Modify: `electron/scheduledTasks/ipc.ts`
- Modify: `electron/scheduledTasks/scheduler.ts`
- Test: `src/App.habitMonitor.test.tsx`
- Test: `electron/scheduledTasks/scheduler.test.ts`

- [ ] **Step 1: Make run-now use current session**

From the renderer, `ScheduledTaskDialog` calls `window.api.scheduledTasks.runNow({ taskId, sessionId, targetRepo, sessionRunning })`.

- [ ] **Step 2: Backend validates session state**

IPC refuses immediate send if no session id is supplied, marks the run queued, and returns a clear warning.

- [ ] **Step 3: Send via controlled AICLI API**

When a session is available, send through existing `cc:send-user` semantics rather than raw PTY input.

- [ ] **Step 4: Surface results**

Show toast messages for queued, sent, failed, and missing-session outcomes.

### Task 5: Verification

**Files:**
- Validate full repository state.

- [ ] **Step 1: Typecheck**

Run: `npm run typecheck`

- [ ] **Step 2: Targeted tests**

Run scheduled-task tests plus updated App test:

```text
npx vitest run electron/scheduledTasks/*.test.ts src/scheduled-tasks/*.test.tsx src/App.habitMonitor.test.tsx
```

- [ ] **Step 3: Broader regression tests**

Run existing related tests for Skill and habit UI:

```text
npx vitest run src/habit/SkillStudioDialog.test.tsx src/habit/SkillGraphDialog.test.tsx electron/habit/*.test.ts
```

- [ ] **Step 4: Manual smoke**

Run the app, open `定时任务`, create a disabled task, edit it, enable it, and use `运行` with the current AICLI session.
