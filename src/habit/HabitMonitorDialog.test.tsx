import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type { ManagedChromeState } from '../../electron/preload'
import type { HabitEventRow, HabitFlowRow, HabitSettings } from './habitTypes'
import HabitMonitorDialog, {
  disableHabitFlow,
  loadHabitMonitorDialogData,
  toggleManagedChromeCollection
} from './HabitMonitorDialog.js'

const settings: HabitSettings = {
  enabled: true,
  kinds: {
    pty_cmd: true,
    panel_open: true,
    site_visit: true,
    site_click: true,
    site_input_hint: true
  },
  retentionDays: 90,
  firstRunNoticeShownAt: 0,
  lastAggregatedAt: 0,
  collectManagedChrome: true,
  autoEnableLowRiskFlows: true,
  autoPersonalizeUi: true
}

const chromeState: ManagedChromeState = {
  running: true,
  port: 9222,
  profileDir: 'E:/tmp/habit-profile',
  pid: 1234,
  lastActiveUrl: 'https://github.com/openai'
}

const flows: HabitFlowRow[] = [
  {
    id: 1,
    kind: 'site-flow',
    title: '打开 GitHub',
    summary: '经常访问 GitHub 仓库',
    evidence_count: 5,
    risk_level: 'low',
    enabled_by_default: 1,
    status: 'active',
    payload: JSON.stringify({ action: 'open-managed-chrome-url', url: 'https://github.com/openai' }),
    created_at: 1,
    updated_at: 1
  },
  {
    id: 2,
    kind: 'site-flow',
    title: '提交周报',
    summary: '疑似会提交远端状态',
    evidence_count: 2,
    risk_level: 'high',
    enabled_by_default: 0,
    status: 'candidate',
    payload: JSON.stringify({ action: 'submit-form' }),
    created_at: 2,
    updated_at: 2
  }
]

const recent: HabitEventRow[] = [
  {
    id: 11,
    ts: 123,
    kind: 'site_visit',
    payload: JSON.stringify({ text: 'https://github.com/openai' }),
    source: 'managed_chrome',
    project_id: 'project-1',
    repo_path: 'E:/OpenSource/multi-ai-code',
    source_window: 'managed-chrome'
  }
]

function createApiMock() {
  return {
    settings: {
      get: vi.fn().mockResolvedValue(settings),
      update: vi.fn().mockResolvedValue({ ...settings, collectManagedChrome: false })
    },
    chrome: {
      getState: vi.fn().mockResolvedValue(chromeState),
      start: vi.fn(),
      stop: vi.fn(),
      focus: vi.fn()
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
  it('loads settings and managed Chrome state when opened', async () => {
    const api = createApiMock()

    const data = await loadHabitMonitorDialogData(api)

    expect(api.settings.get).toHaveBeenCalledOnce()
    expect(api.chrome.getState).toHaveBeenCalledOnce()
    expect(api.flows.list).toHaveBeenCalledOnce()
    expect(data.settings.collectManagedChrome).toBe(true)
    expect(data.chromeState.running).toBe(true)
    expect(data.totalEventCount).toBe(1)
  })

  it('toggles managed Chrome collection through habit settings updates', async () => {
    const api = createApiMock()

    const next = await toggleManagedChromeCollection(api, settings)

    expect(api.settings.update).toHaveBeenCalledWith({ collectManagedChrome: false })
    expect(next.collectManagedChrome).toBe(false)
  })

  it('renders active low-risk flows separately from high-risk candidates', () => {
    const markup = renderToStaticMarkup(
      <HabitMonitorDialog
        onClose={vi.fn()}
        onOpenAiSettings={vi.fn()}
        mainCliLabel="Codex"
        initialData={{
          settings,
          chromeState,
          flows,
          recent,
          totalEventCount: recent.length
        }}
      />
    )

    expect(markup).toContain('习惯监控')
    expect(markup).toContain('托管 Chrome')
    expect(markup).toContain('活跃流程')
    expect(markup).toContain('高风险候选')
    expect(markup).toContain('打开 GitHub')
    expect(markup).toContain('提交周报')
  })

  it('disables a flow from the active list', async () => {
    const api = createApiMock()

    await disableHabitFlow(api, 1)

    expect(api.flows.updateStatus).toHaveBeenCalledWith({ id: 1, status: 'disabled' })
  })
})
