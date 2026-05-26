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
    expect(markup).not.toContain('Chrome')
    expect(markup).not.toContain('Skill')
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
  })

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
