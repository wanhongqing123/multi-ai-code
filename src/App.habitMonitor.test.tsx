import { renderToStaticMarkup } from 'react-dom/server'
import { beforeAll, describe, expect, it, vi } from 'vitest'
import type { HabitFlowRow } from './habit/habitTypes'

describe('App habit monitor integration', () => {
  beforeAll(() => {
    vi.stubGlobal('self', globalThis)
  })

  it('renders habit monitor and managed Chrome in the primary topbar instead of the old entries', async () => {
    const module = await import('./App.js')
    const App = module.default
    const markup = renderToStaticMarkup(<App />)

    expect(markup).toContain('习惯监控')
    expect(markup).toContain('托管 Chrome')
    expect(markup).not.toContain('Skill 学习')
    expect(markup).not.toContain('模板')
    expect(markup).not.toContain('向导')
  })

  it('renders a preload-missing fallback instead of crashing when opened outside Electron', async () => {
    const previousWindow = (globalThis as { window?: unknown }).window
    vi.stubGlobal('window', {})

    try {
      const module = await import('./App.js')
      const App = module.default
      const markup = renderToStaticMarkup(<App />)

      expect(markup).toContain('请通过 Electron 启动')
    } finally {
      if (previousWindow === undefined) {
        delete (globalThis as { window?: unknown }).window
      } else {
        vi.stubGlobal('window', previousWindow)
      }
    }
  })

  it('derives auto-personalized UI flags from active low-risk adjustments', async () => {
    const { deriveHabitUiFlags } = await import('./App.js')
    const flows: HabitFlowRow[] = [
      {
        id: 1,
        kind: 'ui-adjustment',
        title: '隐藏模板入口',
        summary: '模板很少使用',
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
        title: '隐藏向导入口',
        summary: '向导很少使用',
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
