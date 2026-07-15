import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { renderToStaticMarkup } from 'react-dom/server'
import { beforeAll, describe, expect, it, vi } from 'vitest'
import type { HabitFlowRow } from './habit/habitTypes'

describe('App habit monitor integration', () => {
  beforeAll(() => {
    vi.stubGlobal('self', globalThis)
  })

  it('keeps the habit-monitor entry but no longer renders a managed Chrome entry', () => {
    const source = readFileSync(fileURLToPath(new URL('./App.tsx', import.meta.url)), 'utf8')

    expect(source).toContain('Multi-AI Code')
    expect(source).not.toContain('项目记忆')
    expect(source).not.toContain('Chrome')
    expect(source).not.toContain('定时任务管理')
    expect(source).toContain('Skill 管理')
    expect(source).not.toContain('topbar-secondary-row')
    expect(source).not.toContain('skill-bar')
    expect(source).not.toContain('还没有 skill')
  })

  it('places skill orchestration in the same toolbar row as build', () => {
    const source = readFileSync(fileURLToPath(new URL('./App.tsx', import.meta.url)), 'utf8')
    const planRowIndex = source.indexOf('className="plan-name-bar"')
    const scheduledTaskIndex = source.indexOf('setShowScheduledTaskDialog(true)', planRowIndex)
    const buildIndex = source.indexOf('setShowBuildPanel(true)', planRowIndex)
    const skillGraphIndex = source.indexOf('setShowSkillGraphStudio(true)', planRowIndex)

    expect(planRowIndex).toBeGreaterThan(-1)
    expect(scheduledTaskIndex).toBeGreaterThan(planRowIndex)
    expect(buildIndex).toBeGreaterThan(planRowIndex)
    expect(skillGraphIndex).toBeGreaterThan(planRowIndex)
    expect(scheduledTaskIndex).toBeLessThan(skillGraphIndex)
    expect(skillGraphIndex).toBeLessThan(buildIndex)
    expect(source).not.toContain('topbar-secondary-row')
  })

  it('moves habit monitor into settings instead of opening a separate dialog', () => {
    const source = readFileSync(fileURLToPath(new URL('./App.tsx', import.meta.url)), 'utf8')
    const topbarIndex = source.indexOf('<header className="topbar">')
    const topbarEndIndex = source.indexOf('</header>', topbarIndex)
    const topbarSource = source.slice(topbarIndex, topbarEndIndex)
    const settingsDialogIndex = source.indexOf('<AiSettingsDialog')

    expect(topbarIndex).toBeGreaterThan(-1)
    expect(topbarEndIndex).toBeGreaterThan(topbarIndex)
    expect(topbarSource).not.toContain('习惯监控')
    expect(settingsDialogIndex).toBeGreaterThan(-1)
    expect(source).not.toContain("import HabitMonitorDialog")
    expect(source).not.toContain('showHabitMonitor')
    expect(source).not.toContain('setShowHabitMonitor')
    expect(source).not.toContain('<HabitMonitorDialog')
    expect(source).toContain("openAiSettingsSection('habit')")
  })

  it('removes unused topbar sampling/reset entries and the legacy bottom pipeline bar', () => {
    const source = readFileSync(fileURLToPath(new URL('./App.tsx', import.meta.url)), 'utf8')
    const topbarIndex = source.indexOf('<header className="topbar">')
    const topbarEndIndex = source.indexOf('</header>', topbarIndex)
    const topbarSource = source.slice(topbarIndex, topbarEndIndex)

    expect(topbarIndex).toBeGreaterThan(-1)
    expect(topbarEndIndex).toBeGreaterThan(topbarIndex)
    expect(topbarSource).not.toContain('ScreenSamplerIndicator')
    expect(topbarSource).not.toContain('重置主会话')
    expect(source).not.toContain('<footer className="pipeline">')
    expect(source).not.toContain('单阶段架构')
  })

  it('prefers Codex in CLI health checks and keeps Claude optional', () => {
    const source = readFileSync(
      fileURLToPath(new URL('../electron/main.ts', import.meta.url)),
      'utf8'
    )
    const codexToolIndex = source.indexOf("name: 'codex'")
    const claudeToolIndex = source.indexOf("name: 'claude'")
    const claudeToolBlock = source.slice(claudeToolIndex, source.indexOf("cmd: 'claude'", claudeToolIndex))

    expect(codexToolIndex).toBeGreaterThan(-1)
    expect(claudeToolIndex).toBeGreaterThan(-1)
    expect(codexToolIndex).toBeLessThan(claudeToolIndex)
    expect(claudeToolBlock).toContain('required: false')
  })

  it('uses explicit work modes and only loads scheduled tasks in task-watch mode', () => {
    const source = readFileSync(fileURLToPath(new URL('./App.tsx', import.meta.url)), 'utf8')

    expect(source).toContain("type WorkMode = 'task-watch' | 'plan-design'")
    expect(source).toContain('\u5b9a\u65f6\u4efb\u52a1')
    expect(source).toContain('\u666e\u901a\u4efb\u52a1')
    expect(source).toContain('className="topbar-btn mode-toggle-btn"')
    expect(source).toContain("onWorkModeSelect(isTaskWatchMode ? 'plan-design' : 'task-watch')")
    expect(source).toContain('aria-pressed={isTaskWatchMode}')
    expect(source).not.toContain('type="checkbox"')
    expect(source).not.toContain('className="mode-option-group"')
    expect(source).toContain('{isTaskWatchMode && (')
    expect(source).toContain('if (!isTaskWatchMode || !currentProjectId) return')
    expect(source).not.toMatch(/<option\s+value=\{NO_PLAN_SELECT_VALUE\}/)
    expect(source).not.toContain('\u65e0\u65b9\u6848\u6a21\u5f0f')
  })

  it('places mode, normal task entry, and workspace actions in one control row', () => {
    const source = readFileSync(fileURLToPath(new URL('./App.tsx', import.meta.url)), 'utf8')
    const planBarIndex = source.indexOf('className="plan-name-bar"')
    const controlRowIndex = source.indexOf('className="workspace-control-row"', planBarIndex)
    const actionsIndex = source.indexOf('className="workspace-control-actions"', controlRowIndex)
    const normalTaskIndex = source.indexOf('setShowNormalTaskDialog(true)', actionsIndex)
    const planPreviewIndex = source.indexOf('方案预览', actionsIndex)
    const scheduledTaskIndex = source.indexOf('setShowScheduledTaskDialog(true)', actionsIndex)
    const buildIndex = source.indexOf('setShowBuildPanel(true)', actionsIndex)
    const runIndex = source.indexOf('handleStartRuntime()', actionsIndex)

    expect(planBarIndex).toBeGreaterThan(-1)
    expect(controlRowIndex).toBeGreaterThan(planBarIndex)
    expect(actionsIndex).toBeGreaterThan(controlRowIndex)
    expect(normalTaskIndex).toBeGreaterThan(actionsIndex)
    expect(planPreviewIndex).toBeGreaterThan(actionsIndex)
    expect(planPreviewIndex).toBeLessThan(scheduledTaskIndex)
    expect(scheduledTaskIndex).toBeGreaterThan(actionsIndex)
    expect(buildIndex).toBeGreaterThan(actionsIndex)
    expect(runIndex).toBeGreaterThan(actionsIndex)
    expect(source).not.toContain('className="plan-design-main"')
    expect(source).not.toContain('className="plan-select-input"')
    expect(source).not.toContain('placeholder="输入新方案名')
    expect(source).not.toContain('导入外部方案')
  })

  it('groups mode, workspace actions, and toolbar into separate layout clusters', () => {
    const source = readFileSync(fileURLToPath(new URL('./App.tsx', import.meta.url)), 'utf8')

    expect(source).toContain('className="topbar-left"')
    expect(source).toContain('className="topbar-actions"')
    expect(source).toContain('className="workspace-control-row"')
    expect(source).toContain('className="workspace-control-actions"')
    expect(source).toContain('className="topbar-btn mode-toggle-btn"')
    expect(source).toContain('className="plan-toolbar-actions"')
    expect(source).toContain('setShowNormalTaskDialog(true)')
    expect(source).toContain('方案预览')
    expect(source).not.toContain('className="plan-design-main"')
    expect(source).not.toContain('className="plan-design-actions"')
  })

  it('raises plan review above normal task management overlays', () => {
    const dialogSource = readFileSync(
      fileURLToPath(new URL('./components/PlanReviewDialog.tsx', import.meta.url)),
      'utf8'
    )
    const styles = readFileSync(fileURLToPath(new URL('./styles.css', import.meta.url)), 'utf8')

    expect(dialogSource).toContain('modal-backdrop plan-review-backdrop')
    expect(styles).toContain('.plan-review-backdrop')
  })

  it('does not inject normal task content when starting the main session', () => {
    const source = readFileSync(fileURLToPath(new URL('./App.tsx', import.meta.url)), 'utf8')

    expect(source).toContain("planName: ''")
    expect(source).toContain("planMode: 'none'")
    expect(source).toContain('planAbsPath: undefined')
    expect(source).toContain('planPending: false')
    expect(source).toContain('initialUserMessage: undefined')
    expect(source).not.toContain('formatInitialMessage')
  })

  it('runs normal tasks by sending the task prompt to the current AICLI session', () => {
    const source = readFileSync(fileURLToPath(new URL('./App.tsx', import.meta.url)), 'utf8')

    expect(source).toContain('buildNormalTaskRunPrompt')
    expect(source).toContain("showToast('请先启动 AICLI'")
    expect(source).toContain('const prompt = buildNormalTaskRunPrompt(task, targetRepo)')
    expect(source).toContain('window.api.cc.sendUser(sessionId, prompt)')
    expect(source).toContain('onRun={runNormalTask}')
    expect(source).not.toContain('onSelect={selectNormalTask}')
  })

  it('wires normal task metadata saves through the plan api', () => {
    const source = readFileSync(fileURLToPath(new URL('./App.tsx', import.meta.url)), 'utf8')

    expect(source).toContain('window.api.plan.updateMetadata')
    expect(source).toContain('onSaveMetadata={saveNormalTaskMetadata}')
  })

  it('verifies normal task metadata persisted before closing the editor', () => {
    const source = readFileSync(fileURLToPath(new URL('./App.tsx', import.meta.url)), 'utf8')

    expect(source).toContain('verifyNormalTaskMetadataSaved')
    expect(source).toContain('typeof window.api.plan.updateMetadata ===')
    expect(source).toContain('window.api.plan.updateDescription')
    expect(source).toContain('普通任务详情没有写入')
  })

  it('renders a preload-missing fallback instead of crashing when opened outside Electron', async () => {
    const previousWindow = (globalThis as { window?: unknown }).window
    vi.stubGlobal('window', {})

    try {
      const module = await import('./App.js')
      const App = module.default
      const markup = renderToStaticMarkup(<App />)

      expect(markup).toContain('Electron')
      expect(markup).toContain('npm run dev')
    } finally {
      if (previousWindow === undefined) {
        delete (globalThis as { window?: unknown }).window
      } else {
        vi.stubGlobal('window', previousWindow)
      }
    }
  }, 15000)

  it('derives auto-personalized UI flags from active low-risk adjustments', async () => {
    const { deriveHabitUiFlags } = await import('./App.js')
    const flows: HabitFlowRow[] = [
      {
        id: 1,
        kind: 'ui-adjustment',
        title: 'hide templates',
        summary: 'templates are rarely used',
        evidence_count: 4,
        risk_level: 'low',
        enabled_by_default: 1,
        status: 'active',
        payload: JSON.stringify({ action: 'hide-templates-entry' }),
        created_at: 1,
        updated_at: 1
      },
      {
        id: 2,
        kind: 'ui-adjustment',
        title: 'hide wizard',
        summary: 'wizard is rarely used',
        evidence_count: 4,
        risk_level: 'low',
        enabled_by_default: 1,
        status: 'active',
        payload: JSON.stringify({ action: 'hide-wizard-entry' }),
        created_at: 2,
        updated_at: 2
      }
    ]

    expect(
      deriveHabitUiFlags({
        autoPersonalizeUi: true,
        flows
      })
    ).toEqual({
      hideTemplatesEntry: true,
      hideWizardEntry: true
    })
  })
})
