import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { renderToStaticMarkup } from 'react-dom/server'
import { beforeAll, describe, expect, it, vi } from 'vitest'
import type { HabitFlowRow } from './habit/habitTypes'

describe('App habit monitor integration', () => {
  beforeAll(() => {
    vi.stubGlobal('self', globalThis)
  })

  it('keeps the habit-monitor entry but no longer renders a managed Chrome entry', async () => {
    const module = await import('./App.js')
    const App = module.default
    const markup = renderToStaticMarkup(<App />)

    expect(markup).toContain('Multi-AI Code')
    expect(markup).not.toContain('项目记忆')
    expect(markup).not.toContain('Chrome')
    expect(markup).not.toContain('定时任务')
    expect(markup).toContain('Skill 管理')
    expect(markup).not.toContain('Skill 编排')
    expect(markup).not.toContain('topbar-secondary-row')
    expect(markup).not.toContain('skill-bar')
    expect(markup).not.toContain('还没有 skill')
  }, 15000)

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

  it('uses explicit work modes and only loads scheduled tasks in task-watch mode', () => {
    const source = readFileSync(fileURLToPath(new URL('./App.tsx', import.meta.url)), 'utf8')

    expect(source).toContain("type WorkMode = 'task-watch' | 'plan-design'")
    expect(source).toContain('\u4efb\u52a1\u503c\u5b88\u6a21\u5f0f')
    expect(source).toContain('\u65b9\u6848\u8bbe\u8ba1\u6a21\u5f0f')
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

  it('places mode, plan controls, and workspace actions in one control row', () => {
    const source = readFileSync(fileURLToPath(new URL('./App.tsx', import.meta.url)), 'utf8')
    const planBarIndex = source.indexOf('className="plan-name-bar"')
    const controlRowIndex = source.indexOf('className="workspace-control-row"', planBarIndex)
    const controlLeftIndex = source.indexOf('className="workspace-control-left"', controlRowIndex)
    const designConditionIndex = source.indexOf('{isPlanDesignMode && (', controlLeftIndex)
    const designMainIndex = source.indexOf('className="plan-design-main"', designConditionIndex)
    const planLabelIndex = source.indexOf('\u65b9\u6848\uff1a', designMainIndex)
    const planInputIndex = source.indexOf('placeholder="\u8f93\u5165\u65b0\u65b9\u6848\u540d', designMainIndex)
    const actionsIndex = source.indexOf('className="workspace-control-actions"', controlRowIndex)
    const importIndex = source.indexOf('\u5bfc\u5165\u5916\u90e8\u65b9\u6848', actionsIndex)
    const buildIndex = source.indexOf('setShowBuildPanel(true)', actionsIndex)
    const runIndex = source.indexOf('handleStartRuntime()', actionsIndex)

    expect(planBarIndex).toBeGreaterThan(-1)
    expect(controlRowIndex).toBeGreaterThan(planBarIndex)
    expect(controlLeftIndex).toBeGreaterThan(controlRowIndex)
    expect(designConditionIndex).toBeGreaterThan(controlLeftIndex)
    expect(designMainIndex).toBeGreaterThan(designConditionIndex)
    expect(planLabelIndex).toBeGreaterThan(designMainIndex)
    expect(planInputIndex).toBeGreaterThan(designMainIndex)
    expect(actionsIndex).toBeGreaterThan(controlRowIndex)
    expect(importIndex).toBeGreaterThan(actionsIndex)
    expect(buildIndex).toBeGreaterThan(actionsIndex)
    expect(runIndex).toBeGreaterThan(actionsIndex)
  })

  it('groups mode, toolbar, plan inputs, and plan actions into separate layout clusters', () => {
    const source = readFileSync(fileURLToPath(new URL('./App.tsx', import.meta.url)), 'utf8')

    expect(source).toContain('className="topbar-left"')
    expect(source).toContain('className="topbar-actions"')
    expect(source).toContain('className="workspace-control-row"')
    expect(source).toContain('className="workspace-control-left"')
    expect(source).toContain('className="workspace-control-actions"')
    expect(source).toContain('className="topbar-btn mode-toggle-btn"')
    expect(source).toContain('className="plan-toolbar-actions"')
    expect(source).toContain('className="plan-design-main"')
    expect(source).toContain('className="plan-design-actions"')
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
