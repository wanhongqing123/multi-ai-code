import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type { HabitEventRow, HabitFlowRow, HabitSettings } from './habitTypes'
import CollectionSettingsPanel from './CollectionSettingsPanel.js'
import HabitMonitorDialog, {
  disableHabitFlow,
  loadHabitMonitorDialogData
} from './HabitMonitorDialog.js'

const settings: HabitSettings = {
  enabled: true,
  kinds: {
    pty_cmd: true,
    panel_open: true,
    action_triggered: true,
    screen_window: true,
    screen_frame: true
  },
  retentionDays: 90,
  firstRunNoticeShownAt: 0,
  lastAggregatedAt: 0,
  autoEnableLowRiskFlows: true,
  autoPersonalizeUi: true,
  screenSampler: {
    enabled: true,
    paused: false,
    appBlocklist: []
  }
}

const flows: HabitFlowRow[] = [
  {
    id: 1,
    kind: 'app-flow',
    title: 'Open Build Panel',
    summary: 'Frequently opens the build panel',
    evidence_count: 5,
    risk_level: 'low',
    enabled_by_default: 1,
    status: 'active',
    payload: JSON.stringify({ action: 'open-panel', panelKey: 'build' }),
    created_at: 1,
    updated_at: 1
  },
  {
    id: 2,
    kind: 'ui-adjustment',
    title: 'Hide Templates Entry',
    summary: 'Templates is low-frequency',
    evidence_count: 2,
    risk_level: 'low',
    enabled_by_default: 1,
    status: 'candidate',
    payload: JSON.stringify({ action: 'hide-templates-entry' }),
    created_at: 2,
    updated_at: 2
  }
]

const recent: HabitEventRow[] = [
  {
    id: 11,
    ts: 123,
    kind: 'screen_window',
    payload: JSON.stringify({ text: 'QQ - team chat' }),
    source: null,
    project_id: 'project-1',
    repo_path: 'E:/OpenSource/multi-ai-code',
    source_window: 'screen-sampler'
  }
]

function createApiMock() {
  return {
    settings: {
      get: vi.fn().mockResolvedValue(settings),
      update: vi.fn().mockResolvedValue(settings)
    },
    flows: {
      list: vi.fn().mockResolvedValue(flows),
      updateStatus: vi.fn().mockResolvedValue({ ok: true }),
      clear: vi.fn()
    },
    events: {
      recent: vi.fn().mockResolvedValue({ events: recent, total: recent.length }),
      clear: vi.fn()
    },
    runNow: vi.fn()
  }
}

describe('HabitMonitorDialog', () => {
  it('loads settings and recent events when opened', async () => {
    const api = createApiMock()

    const data = await loadHabitMonitorDialogData(api)

    expect(api.settings.get).toHaveBeenCalledOnce()
    expect(api.flows.list).toHaveBeenCalledOnce()
    expect(data.totalEventCount).toBe(1)
    expect(data.flows).toHaveLength(2)
  })

  it('renders flows without a managed Chrome panel', () => {
    const markup = renderToStaticMarkup(
      <HabitMonitorDialog
        onClose={vi.fn()}
        onOpenAiSettings={vi.fn()}
        mainCliLabel="Codex"
        initialData={{
          settings,
          flows,
          recent,
          totalEventCount: recent.length
        }}
      />
    )

    expect(markup).toContain('Open Build Panel')
    expect(markup).not.toContain('Chrome')
  })

  it('renders only the screenshot sampling kind in collection settings', () => {
    const markup = renderToStaticMarkup(
      <CollectionSettingsPanel
        settings={settings}
        onUpdate={vi.fn()}
        recent={recent}
        totalEventCount={recent.length}
        onRefresh={vi.fn()}
        onClearEvents={vi.fn()}
      />
    )

    expect(markup).toContain('屏幕截图采样')
    expect(markup).not.toContain('前台窗口采样')
    expect(markup).not.toContain('主会话终端命令')
    expect(markup).not.toContain('主会话 AI prompt')
  })

  it('disables a flow from the active list', async () => {
    const api = createApiMock()

    await disableHabitFlow(api, 1)

    expect(api.flows.updateStatus).toHaveBeenCalledWith({ id: 1, status: 'disabled' })
  })
})
