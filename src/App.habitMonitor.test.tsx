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
