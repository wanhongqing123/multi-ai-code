# Settings JSON Repair And Shortcut Presets Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Automatically repair recoverable `project.json` corruption during settings saves, and update the screenshot shortcut UI to prefer preset combinations with expandable custom input.

**Architecture:** Extract project-metadata parsing and auto-repair into a focused main-process helper so `project:set-ai-settings` and `project:set-repo-view-ai-settings` stop parsing raw JSON inline. Keep screenshot shortcut registration unchanged in the main process; only reshape the renderer-side settings dialog so users pick from preset buttons first and only expand a custom text field on demand.

**Tech Stack:** Electron 33, React 18, TypeScript, Vitest, Node `fs/promises`, existing `window.api` preload bridge, existing modal/styles system.

---

## File Structure

- Create: `electron/store/projectMeta.ts`
  - Safe `project.json` read/repair helper, first-object extraction, backup creation, and structured result types.
- Create: `electron/store/projectMeta.test.ts`
  - Unit tests for valid JSON, trailing-garbage auto-repair, backup creation, and unrecoverable corruption.
- Modify: `electron/main.ts`
  - Replace inline `JSON.parse(project.json)` save logic with `projectMeta` helper and structured user-facing errors.
- Modify: `src/components/AiSettingsDialog.tsx`
  - Add screenshot shortcut preset buttons, expandable custom input, and app/project save feedback plumbing.
- Modify: `src/components/AiSettingsDialog.test.tsx`
  - Add tests for preset highlighting, custom toggle visibility, fallback expansion, and restore-default behavior.
- Modify: `src/styles.css`
  - Add layout and state styles for the shortcut preset button row and custom editor area.

## Task 1: Add Safe `project.json` Read/Repair Helper

**Files:**
- Create: `electron/store/projectMeta.ts`
- Create: `electron/store/projectMeta.test.ts`

- [ ] **Step 1: Write the failing repair tests**

```ts
import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import {
  type ProjectMetaReadResult,
  readProjectMetaFile,
  writeProjectMetaFile
} from './projectMeta.js'

let root: string
let projectDir: string
let metaPath: string

beforeEach(async () => {
  root = await fs.mkdtemp(join(tmpdir(), 'mac-project-meta-'))
  projectDir = join(root, 'project')
  metaPath = join(projectDir, 'project.json')
  await fs.mkdir(projectDir, { recursive: true })
})

afterEach(async () => {
  await fs.rm(root, { recursive: true, force: true })
})

describe('readProjectMetaFile', () => {
  it('reads valid project.json without repair', async () => {
    await fs.writeFile(
      metaPath,
      JSON.stringify({ id: 'p1', name: 'demo', target_repo: 'C:/repo' }, null, 2),
      'utf8'
    )

    const result = await readProjectMetaFile(metaPath)

    expect(result).toEqual({
      ok: true,
      repaired: false,
      meta: { id: 'p1', name: 'demo', target_repo: 'C:/repo' }
    })
  })

  it('auto-repairs when trailing garbage follows a complete object', async () => {
    await fs.writeFile(
      metaPath,
      '{\n  "id": "p2",\n  "name": "demo"\n}  "ai_settings": { "ai_cli": "claude" }\n',
      'utf8'
    )

    const result = await readProjectMetaFile(metaPath)
    const repaired = JSON.parse(await fs.readFile(metaPath, 'utf8'))
    const backups = await fs.readdir(projectDir)

    expect(result.ok).toBe(true)
    if (result.ok) {
      expect(result.repaired).toBe(true)
      expect(result.meta).toEqual({ id: 'p2', name: 'demo' })
    }
    expect(repaired).toEqual({ id: 'p2', name: 'demo' })
    expect(backups.some((name) => name.startsWith('project.json.autofix-'))).toBe(true)
  })

  it('returns a structured unrecoverable error for broken JSON bodies', async () => {
    await fs.writeFile(metaPath, '{\n  "id": "p3",\n  "name": \n', 'utf8')

    const result = await readProjectMetaFile(metaPath)

    expect(result).toEqual({
      ok: false,
      repaired: false,
      error: 'project settings corrupted and unrecoverable'
    })
  })
})

describe('writeProjectMetaFile', () => {
  it('writes the updated metadata back with stable formatting', async () => {
    await writeProjectMetaFile(metaPath, {
      id: 'p4',
      name: 'demo',
      ai_settings: { ai_cli: 'claude', env: {} }
    })

    await expect(fs.readFile(metaPath, 'utf8')).resolves.toBe(
      '{\n' +
        '  "id": "p4",\n' +
        '  "name": "demo",\n' +
        '  "ai_settings": {\n' +
        '    "ai_cli": "claude",\n' +
        '    "env": {}\n' +
        '  }\n' +
        '}'
    )
  })
})
```

- [ ] **Step 2: Run the new test to verify it fails**

Run: `node .\node_modules\vitest\vitest.mjs run electron/store/projectMeta.test.ts`

Expected: FAIL with module-not-found errors for `./projectMeta.js`.

- [ ] **Step 3: Implement the minimal helper module**

```ts
import { promises as fs } from 'fs'
import { dirname, join } from 'path'

export type ProjectMeta = Record<string, unknown>

export type ProjectMetaReadResult =
  | { ok: true; repaired: boolean; meta: ProjectMeta }
  | { ok: false; repaired: false; error: 'project settings corrupted and unrecoverable' }

function tryParseObject(raw: string): ProjectMeta | null {
  const trimmed = raw.trimStart()
  if (!trimmed.startsWith('{')) return null
  let depth = 0
  let inString = false
  let escaped = false
  const start = raw.indexOf('{')
  for (let i = start; i < raw.length; i += 1) {
    const ch = raw[i]
    if (inString) {
      if (escaped) {
        escaped = false
      } else if (ch === '\\') {
        escaped = true
      } else if (ch === '"') {
        inString = false
      }
      continue
    }
    if (ch === '"') {
      inString = true
      continue
    }
    if (ch === '{') depth += 1
    if (ch === '}') {
      depth -= 1
      if (depth === 0) {
        try {
          return JSON.parse(raw.slice(start, i + 1)) as ProjectMeta
        } catch {
          return null
        }
      }
    }
  }
  return null
}

async function writeBackup(metaPath: string, raw: string): Promise<void> {
  const stamp = new Date().toISOString().replace(/[:.]/g, '-')
  const backupPath = join(dirname(metaPath), `project.json.autofix-${stamp}.bak`)
  await fs.writeFile(backupPath, raw, 'utf8')
}

export async function readProjectMetaFile(metaPath: string): Promise<ProjectMetaReadResult> {
  const raw = await fs.readFile(metaPath, 'utf8')
  try {
    return { ok: true, repaired: false, meta: JSON.parse(raw) as ProjectMeta }
  } catch {
    const repaired = tryParseObject(raw)
    if (!repaired) {
      return { ok: false, repaired: false, error: 'project settings corrupted and unrecoverable' }
    }
    await writeBackup(metaPath, raw)
    await writeProjectMetaFile(metaPath, repaired)
    return { ok: true, repaired: true, meta: repaired }
  }
}

export async function writeProjectMetaFile(metaPath: string, meta: ProjectMeta): Promise<void> {
  await fs.writeFile(metaPath, JSON.stringify(meta, null, 2), 'utf8')
}
```

- [ ] **Step 4: Run the helper test to verify it passes**

Run: `node .\node_modules\vitest\vitest.mjs run electron/store/projectMeta.test.ts`

Expected: PASS

- [ ] **Step 5: Commit the helper layer**

```bash
git add electron/store/projectMeta.ts electron/store/projectMeta.test.ts
git commit -m "feat: repair corrupted project settings metadata"
```

## Task 2: Wire Project-Settings Saves Through The Repair Helper

**Files:**
- Modify: `electron/main.ts`
- Test: `electron/store/projectMeta.test.ts`

- [ ] **Step 1: Extend the helper test with save-path coverage that reproduces the user-visible corruption**

```ts
it('preserves existing fields while applying ai_settings after auto-repair', async () => {
  await fs.writeFile(
    metaPath,
    '{\n  "id": "p5",\n  "name": "demo",\n  "repo_view_ai_settings": {\n    "ai_cli": "claude"\n  }\n}  "ai_settings": { "ai_cli": "codex" }\n',
    'utf8'
  )

  const read = await readProjectMetaFile(metaPath)
  expect(read.ok).toBe(true)
  if (!read.ok) return

  read.meta.ai_settings = { ai_cli: 'claude', env: {} }
  await writeProjectMetaFile(metaPath, read.meta)

  await expect(fs.readFile(metaPath, 'utf8')).resolves.toContain('"repo_view_ai_settings"')
  await expect(fs.readFile(metaPath, 'utf8')).resolves.toContain('"ai_settings"')
})
```

- [ ] **Step 2: Run the helper test again**

Run: `node .\node_modules\vitest\vitest.mjs run electron/store/projectMeta.test.ts`

Expected: PASS

- [ ] **Step 3: Update `electron/main.ts` to use the helper in both save handlers**

```ts
import {
  readProjectMetaFile,
  writeProjectMetaFile,
  type ProjectMeta
} from './store/projectMeta.js'
```

```ts
ipcMain.handle(
  'project:set-ai-settings',
  async (
    _e,
    { id, settings }: { id: string; settings: AiSettings }
  ): Promise<{ ok: boolean; error?: string; repaired?: boolean }> => {
    const metaPath = join(projectDirFn(id), 'project.json')
    try {
      const read = await readProjectMetaFile(metaPath)
      if (!read.ok) {
        return { ok: false, error: read.error }
      }
      const meta = { ...read.meta, ai_settings: settings as unknown as ProjectMeta }
      await writeProjectMetaFile(metaPath, meta)
      return { ok: true, repaired: read.repaired }
    } catch (err: unknown) {
      return {
        ok: false,
        error: err instanceof Error ? err.message : String(err)
      }
    }
  }
)
```

```ts
ipcMain.handle(
  'project:set-repo-view-ai-settings',
  async (
    _e,
    { id, settings }: { id: string; settings: AiSettings }
  ): Promise<{ ok: boolean; error?: string; repaired?: boolean }> => {
    const metaPath = join(projectDirFn(id), 'project.json')
    try {
      const read = await readProjectMetaFile(metaPath)
      if (!read.ok) {
        return { ok: false, error: read.error }
      }
      const meta = {
        ...read.meta,
        repo_view_ai_settings: settings as unknown as ProjectMeta
      }
      await writeProjectMetaFile(metaPath, meta)
      return { ok: true, repaired: read.repaired }
    } catch (err: unknown) {
      return {
        ok: false,
        error: err instanceof Error ? err.message : String(err)
      }
    }
  }
)
```

- [ ] **Step 4: Run typecheck and the helper test**

Run: `npm.cmd run typecheck`

Expected: PASS

Run: `node .\node_modules\vitest\vitest.mjs run electron/store/projectMeta.test.ts`

Expected: PASS

- [ ] **Step 5: Commit the main-process integration**

```bash
git add electron/main.ts electron/store/projectMeta.ts electron/store/projectMeta.test.ts
git commit -m "fix: auto-repair corrupted project settings files"
```

## Task 3: Add Screenshot Shortcut Preset Buttons And Expandable Custom Input

**Files:**
- Modify: `src/components/AiSettingsDialog.tsx`
- Modify: `src/components/AiSettingsDialog.test.tsx`

- [ ] **Step 1: Write the failing renderer tests for preset and custom modes**

```ts
it('renders preset labels and hides the custom field for a preset value', () => {
  const markup = renderToStaticMarkup(
    <AiSettingsDialog
      projectId="project-1"
      initial={{ ai_cli: 'claude' }}
      initialRepoView={{ ai_cli: 'claude' }}
      initialAppSettings={{
        screenshotShortcutEnabled: true,
        screenshotShortcut: 'CommandOrControl+Shift+S'
      }}
      onClose={vi.fn()}
      onSaved={vi.fn()}
      onSavedRepoView={vi.fn()}
      onSavedAppSettings={vi.fn()}
    />
  )

  expect(markup).toContain('Ctrl/Cmd + Shift + A')
  expect(markup).toContain('Ctrl/Cmd + Shift + S')
  expect(markup).toContain('自定义')
  expect(markup).not.toContain('placeholder="CommandOrControl+Shift+A"')
})

it('shows the custom field when the current shortcut is not one of the presets', () => {
  const markup = renderToStaticMarkup(
    <AiSettingsDialog
      projectId="project-1"
      initial={{ ai_cli: 'claude' }}
      initialRepoView={{ ai_cli: 'claude' }}
      initialAppSettings={{
        screenshotShortcutEnabled: true,
        screenshotShortcut: 'CommandOrControl+Alt+X'
      }}
      onClose={vi.fn()}
      onSaved={vi.fn()}
      onSavedRepoView={vi.fn()}
      onSavedAppSettings={vi.fn()}
    />
  )

  expect(markup).toContain('placeholder="CommandOrControl+Shift+A"')
  expect(markup).toContain('value="CommandOrControl+Alt+X"')
})
```

- [ ] **Step 2: Run the settings dialog test to verify it fails**

Run: `node .\node_modules\vitest\vitest.mjs run src/components/AiSettingsDialog.test.tsx`

Expected: FAIL because the preset labels and custom-toggle behavior do not exist yet.

- [ ] **Step 3: Implement preset constants, active-state helpers, and custom-toggle state**

```ts
const DEFAULT_SCREENSHOT_SHORTCUT = 'CommandOrControl+Shift+A'

const SCREENSHOT_SHORTCUT_PRESETS = [
  { label: 'Ctrl/Cmd + Shift + A', value: 'CommandOrControl+Shift+A' },
  { label: 'Ctrl/Cmd + Shift + S', value: 'CommandOrControl+Shift+S' },
  { label: 'Ctrl/Cmd + Alt + A', value: 'CommandOrControl+Alt+A' },
  { label: 'Alt + Shift + A', value: 'Alt+Shift+A' }
] as const

function isPresetShortcut(value: string): boolean {
  return SCREENSHOT_SHORTCUT_PRESETS.some((preset) => preset.value === value)
}
```

```ts
const [showCustomShortcutInput, setShowCustomShortcutInput] = useState<boolean>(
  !isPresetShortcut(props.initialAppSettings.screenshotShortcut)
)
```

```ts
useEffect(() => {
  if (
    !shouldApplyIncomingAppSettings(
      lastSyncedAppSettingsRef.current,
      props.initialAppSettings,
      saving
    )
  ) {
    return
  }
  setScreenshotShortcutEnabled(props.initialAppSettings.screenshotShortcutEnabled)
  setScreenshotShortcut(props.initialAppSettings.screenshotShortcut)
  setShowCustomShortcutInput(!isPresetShortcut(props.initialAppSettings.screenshotShortcut))
  lastSyncedAppSettingsRef.current = props.initialAppSettings
}, [props.initialAppSettings, saving])
```

```ts
const restoreDefaultShortcut = (): void => {
  setScreenshotShortcutEnabled(true)
  setScreenshotShortcut(DEFAULT_SCREENSHOT_SHORTCUT)
  setShowCustomShortcutInput(false)
}
```

- [ ] **Step 4: Replace the single text input with preset buttons plus custom toggle**

```tsx
<div className="ai-settings-shortcut-presets">
  {SCREENSHOT_SHORTCUT_PRESETS.map((preset) => (
    <button
      key={preset.value}
      type="button"
      className={`drawer-btn ai-settings-shortcut-chip${
        screenshotShortcut === preset.value ? ' is-active' : ''
      }`}
      disabled={props.disabled}
      onClick={() => {
        props.onShortcutChange(preset.value)
        props.onCustomToggle(false)
      }}
    >
      {preset.label}
    </button>
  ))}
  <button
    type="button"
    className={`drawer-btn ai-settings-shortcut-chip${
      props.showCustomShortcutInput ? ' is-active' : ''
    }`}
    disabled={props.disabled}
    onClick={() => props.onCustomToggle(true)}
  >
    自定义
  </button>
</div>
{props.showCustomShortcutInput && (
  <label>
    自定义快捷键
    <input
      type="text"
      value={props.shortcut}
      onChange={(e) => props.onShortcutChange(e.target.value)}
      placeholder={DEFAULT_SCREENSHOT_SHORTCUT}
      disabled={props.disabled}
    />
  </label>
)}
```

- [ ] **Step 5: Run the settings dialog test to verify it passes**

Run: `node .\node_modules\vitest\vitest.mjs run src/components/AiSettingsDialog.test.tsx`

Expected: PASS

- [ ] **Step 6: Commit the renderer logic**

```bash
git add src/components/AiSettingsDialog.tsx src/components/AiSettingsDialog.test.tsx
git commit -m "feat: add screenshot shortcut preset controls"
```

## Task 4: Surface Auto-Repair Feedback And Finish The Styles

**Files:**
- Modify: `src/components/AiSettingsDialog.tsx`
- Modify: `src/styles.css`
- Modify: `src/components/AiSettingsDialog.test.tsx`

- [ ] **Step 1: Add the failing UI feedback tests**

```ts
it('shows the custom field when the custom toggle is active', () => {
  const markup = renderToStaticMarkup(
    <AiSettingsDialog
      projectId={null}
      initial={{ ai_cli: 'claude' }}
      initialRepoView={{ ai_cli: 'claude' }}
      initialAppSettings={{
        screenshotShortcutEnabled: true,
        screenshotShortcut: 'CommandOrControl+Alt+X'
      }}
      onClose={vi.fn()}
      onSaved={vi.fn()}
      onSavedRepoView={vi.fn()}
      onSavedAppSettings={vi.fn()}
    />
  )

  expect(markup).toContain('自定义快捷键')
})
```

- [ ] **Step 2: Run the dialog test to verify it still captures the intended failure surface**

Run: `node .\node_modules\vitest\vitest.mjs run src/components/AiSettingsDialog.test.tsx`

Expected: FAIL until the final props and markup are wired.

- [ ] **Step 3: Add user-facing repair feedback and final prop wiring in the dialog**

```ts
const [notice, setNotice] = useState<string | null>(null)
```

```ts
if (props.projectId) {
  const [mainRes, repoRes] = await Promise.all([
    window.api.project.setAiSettings(props.projectId, nextMain),
    window.api.project.setRepoViewAiSettings(props.projectId, nextRepoView)
  ])
  if (!mainRes.ok) throw new Error(mainRes.error ?? 'save main settings failed')
  if (!repoRes.ok) throw new Error(repoRes.error ?? 'save repo-view settings failed')
  if (mainRes.repaired || repoRes.repaired) {
    setNotice('项目设置文件已自动修复并保存')
  }
  props.onSaved(nextMain)
  props.onSavedRepoView(nextRepoView)
}
```

```tsx
{notice && <div className="ai-settings-notice">{notice}</div>}
{error && <div className="modal-error">⚠ {error}</div>}
```

- [ ] **Step 4: Add the preset/custom styles**

```css
.ai-settings-shortcut-presets {
  display: flex;
  flex-wrap: wrap;
  gap: var(--mac-sp-2);
}

.ai-settings-shortcut-chip {
  min-height: 34px;
}

.ai-settings-shortcut-chip.is-active {
  background: var(--mac-primary-soft);
  border-color: var(--mac-primary);
  color: var(--mac-primary);
}

.ai-settings-shortcut-custom {
  margin-top: var(--mac-sp-3);
}

.ai-settings-notice {
  color: var(--mac-primary);
  background: var(--mac-primary-soft);
  border: 1px solid var(--mac-primary);
  border-radius: var(--mac-r-md);
  padding: var(--mac-sp-2) var(--mac-sp-3);
  font-size: var(--mac-text-sm);
}
```

- [ ] **Step 5: Run full verification for this feature slice**

Run: `npm.cmd run typecheck`

Expected: PASS

Run: `node .\node_modules\vitest\vitest.mjs run electron/store/projectMeta.test.ts src/components/AiSettingsDialog.test.tsx`

Expected: PASS

- [ ] **Step 6: Commit the feedback + style pass**

```bash
git add src/components/AiSettingsDialog.tsx src/components/AiSettingsDialog.test.tsx src/styles.css electron/main.ts electron/store/projectMeta.ts electron/store/projectMeta.test.ts
git commit -m "fix: repair settings metadata and streamline shortcut editing"
```

## Task 5: Final Manual Verification

**Files:**
- Modify: none

- [ ] **Step 1: Verify a corrupted `project.json` is auto-repaired**

Run:

```powershell
@'
{
  "id": "p_bad",
  "name": "demo",
  "target_repo": "C:\\repo"
}  "ai_settings": { "ai_cli": "claude" }
'@ | Set-Content "$env:USERPROFILE\MultiAICode\projects\p_bad\project.json"
```

Expected: File now reproduces the same tail-garbage pattern as the bug report.

- [ ] **Step 2: Open the app and save settings for that project**

Run: `npm.cmd run dev`

Expected: Settings save succeeds, the dialog does not show raw JSON parse text, and the app surfaces a friendly repair message.

- [ ] **Step 3: Confirm the repaired file and backup exist**

Run:

```powershell
Get-Content "$env:USERPROFILE\MultiAICode\projects\p_bad\project.json" -Raw
Get-ChildItem "$env:USERPROFILE\MultiAICode\projects\p_bad" -Filter "project.json.autofix-*.bak"
```

Expected: `project.json` is valid JSON with a single object, and at least one `.bak` file exists.

- [ ] **Step 4: Verify preset and custom shortcut behavior manually**

Run: `npm.cmd run dev`

Expected:
- Clicking `Ctrl/Cmd + Shift + S` selects it without showing the custom field.
- Clicking `自定义` expands the text field.
- Clicking `恢复默认` selects `Ctrl/Cmd + Shift + A` and hides the custom field again.

- [ ] **Step 5: Final integration commit status check**

Run: `git status --short`

Expected: Clean working tree after all commits in this plan.

## Self-Review Checklist

- Spec coverage:
  - `project.json` auto-repair is implemented in Tasks 1-2 and manually verified in Task 5.
  - Preset shortcut buttons and expandable custom input are implemented in Tasks 3-4 and manually verified in Task 5.
  - Friendly user feedback replaces raw JSON parse errors in Task 4.
- Placeholder scan:
  - No `TODO`, `TBD`, or “appropriate handling” placeholders remain.
- Type consistency:
  - `readProjectMetaFile`, `writeProjectMetaFile`, `showCustomShortcutInput`, and `repaired` are used consistently across all tasks.
