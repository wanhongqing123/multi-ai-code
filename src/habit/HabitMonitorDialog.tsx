import { useCallback, useEffect, useMemo, useState } from 'react'
import CollectionSettingsPanel from './CollectionSettingsPanel'
import FlowsPanel from './FlowsPanel'
import type { HabitEventRow, HabitFlowRow, HabitSettings } from './habitTypes'

type HabitFlowStatus = 'candidate' | 'active' | 'disabled'

export interface HabitMonitorApi {
  settings: {
    get: () => Promise<HabitSettings>
    update: (patch: Partial<HabitSettings>) => Promise<HabitSettings>
  }
  flows: {
    list: (opts?: { statuses?: HabitFlowStatus[]; limit?: number }) => Promise<HabitFlowRow[]>
    updateStatus: (req: { id: number; status: HabitFlowStatus }) => Promise<{ ok: boolean }>
  }
  events: {
    recent: (limit?: number) => Promise<{ events: HabitEventRow[]; total: number }>
    clear: () => Promise<{ ok: boolean; removed: number }>
  }
}

export interface HabitMonitorDialogData {
  settings: HabitSettings
  flows: HabitFlowRow[]
  recent: HabitEventRow[]
  totalEventCount: number
}

interface Props {
  onClose: () => void
  onOpenAiSettings: () => void
  mainCliLabel: string
  api?: HabitMonitorApi
  initialData?: HabitMonitorDialogData
}

export interface HabitMonitorPanelProps {
  onOpenAiSettings?: () => void
  mainCliLabel: string
  api?: HabitMonitorApi
  initialData?: HabitMonitorDialogData
}

type Tab = 'overview' | 'collection'

function getDefaultApi(): HabitMonitorApi {
  return window.api.habit as HabitMonitorApi
}

export async function loadHabitMonitorDialogData(
  api: HabitMonitorApi,
  recentLimit = 60
): Promise<HabitMonitorDialogData> {
  const [settings, flows, recentResult] = await Promise.all([
    api.settings.get(),
    api.flows.list({ statuses: ['active', 'candidate', 'disabled'], limit: 200 }),
    api.events.recent(recentLimit)
  ])

  return {
    settings,
    flows,
    recent: recentResult.events,
    totalEventCount: recentResult.total
  }
}

export async function disableHabitFlow(api: HabitMonitorApi, id: number): Promise<void> {
  await api.flows.updateStatus({ id, status: 'disabled' })
}

function ToggleRow(props: {
  label: string
  hint: string
  checked: boolean
  disabled: boolean
  onChange: () => void | Promise<void>
}): JSX.Element {
  const { label, hint, checked, disabled, onChange } = props
  return (
    <label className="habit-overview-toggle">
      <input
        type="checkbox"
        checked={checked}
        disabled={disabled}
        onChange={() => void onChange()}
      />
      <span className="habit-overview-toggle-copy">
        <strong>{label}</strong>
        <span className="habit-settings-hint">{hint}</span>
      </span>
    </label>
  )
}

export function HabitMonitorPanel(props: HabitMonitorPanelProps): JSX.Element {
  const { onOpenAiSettings, mainCliLabel, api, initialData } = props
  const habitApi = api ?? (typeof window !== 'undefined' ? getDefaultApi() : null)
  const [tab, setTab] = useState<Tab>('overview')
  const [data, setData] = useState<HabitMonitorDialogData | null>(initialData ?? null)
  const [loading, setLoading] = useState(!initialData)
  const [busy, setBusy] = useState(false)
  const [toast, setToast] = useState<string | null>(null)

  const flashToast = useCallback((message: string) => {
    setToast(message)
    setTimeout(() => setToast(null), 2500)
  }, [])

  const loadAll = useCallback(async () => {
    if (!habitApi) return
    setLoading(true)
    try {
      setData(await loadHabitMonitorDialogData(habitApi))
    } finally {
      setLoading(false)
    }
  }, [habitApi])

  const refreshEvents = useCallback(async () => {
    if (!habitApi) return
    const result = await habitApi.events.recent(60)
    setData((current) =>
      current
        ? {
            ...current,
            recent: result.events,
            totalEventCount: result.total
          }
        : current
    )
  }, [habitApi])

  const refreshFlows = useCallback(async () => {
    if (!habitApi) return
    const flows = await habitApi.flows.list({
      statuses: ['active', 'candidate', 'disabled'],
      limit: 200
    })
    setData((current) => (current ? { ...current, flows } : current))
  }, [habitApi])

  useEffect(() => {
    if (initialData) return
    void loadAll()
  }, [initialData, loadAll])

  const settings = data?.settings ?? null
  const flows = data?.flows ?? []
  const recent = data?.recent ?? []
  const totalEventCount = data?.totalEventCount ?? 0

  const activeCount = useMemo(
    () => flows.filter((flow) => flow.status === 'active').length,
    [flows]
  )

  const handleSettingsUpdate = useCallback(
    async (patch: Partial<HabitSettings>) => {
      if (!habitApi || !settings) return
      setBusy(true)
      try {
        const next = await habitApi.settings.update(patch)
        setData((current) => (current ? { ...current, settings: next } : current))
      } finally {
        setBusy(false)
      }
    },
    [habitApi, settings]
  )

  const handleClearEvents = useCallback(async () => {
    if (!habitApi) return
    setBusy(true)
    try {
      const result = await habitApi.events.clear()
      flashToast(`已清空 ${result.removed} 条原始事件`)
      await refreshEvents()
    } finally {
      setBusy(false)
    }
  }, [habitApi, flashToast, refreshEvents])

  const handleDisableFlow = useCallback(
    async (id: number) => {
      if (!habitApi) return
      setBusy(true)
      try {
        await disableHabitFlow(habitApi, id)
        await refreshFlows()
      } finally {
        setBusy(false)
      }
    },
    [habitApi, refreshFlows]
  )

  return (
    <section className="habit-monitor-settings-panel habit-monitor-panel">
      <div className="habit-source-hint">
        当前主会话 AI CLI：<strong>{mainCliLabel}</strong>。监控和自动化都沿用这套主配置。
        {' '}
        {onOpenAiSettings ? (
          <button type="button" className="habit-source-link" onClick={onOpenAiSettings}>
            打开设置
          </button>
        ) : null}
      </div>

      <div className="habit-tabs">
        <button
          type="button"
          className={`habit-tab-btn ${tab === 'overview' ? 'active' : ''}`}
          onClick={() => setTab('overview')}
        >
          总览
        </button>
        <button
          type="button"
          className={`habit-tab-btn ${tab === 'collection' ? 'active' : ''}`}
          onClick={() => setTab('collection')}
        >
          原始采集
        </button>
      </div>

      <div className="habit-tab-body">
        {loading || !settings ? (
          <div className="drawer-empty">加载习惯监控状态中...</div>
        ) : null}

        {!loading && settings && tab === 'overview' ? (
          <div className="habit-monitor-panel">
            <section className="habit-settings-section">
              <header className="habit-settings-section-head">
                <div>
                  <strong>监控策略</strong>
                  <div className="habit-settings-hint">
                    当前共有 {activeCount} 条活跃流程，原始事件 {totalEventCount} 条。
                  </div>
                </div>
              </header>

              <div className="habit-overview-toggle-list">
                <ToggleRow
                  label="自动启用低风险流程"
                  hint="如打开常用页面、切换面板这类低风险动作会直接进入活跃列表。"
                  checked={settings.autoEnableLowRiskFlows}
                  disabled={busy}
                  onChange={() =>
                    void handleSettingsUpdate({
                      autoEnableLowRiskFlows: !settings.autoEnableLowRiskFlows
                    })
                  }
                />
                <ToggleRow
                  label="自动个性化界面"
                  hint="低频入口会从主导航淡出，但仍保留在次级入口里。"
                  checked={settings.autoPersonalizeUi}
                  disabled={busy}
                  onChange={() =>
                    void handleSettingsUpdate({
                      autoPersonalizeUi: !settings.autoPersonalizeUi
                    })
                  }
                />
              </div>
            </section>

            <FlowsPanel flows={flows} busy={busy} onDisable={handleDisableFlow} />
          </div>
        ) : null}

        {!loading && settings && tab === 'collection' ? (
          <CollectionSettingsPanel
            settings={settings}
            onUpdate={handleSettingsUpdate}
            recent={recent}
            totalEventCount={totalEventCount}
            onRefresh={refreshEvents}
            onClearEvents={handleClearEvents}
          />
        ) : null}
      </div>

      {toast ? <div className="habit-toast">{toast}</div> : null}
    </section>
  )
}

export default function HabitMonitorDialog(props: Props): JSX.Element {
  const { onClose, ...panelProps } = props

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div
        className="modal templates-modal habit-monitor-modal"
        onClick={(event) => event.stopPropagation()}
      >
        <div className="modal-head">
          <h3>🧠 习惯监控</h3>
          <button className="modal-close" onClick={onClose}>
            ×
          </button>
        </div>

        <HabitMonitorPanel {...panelProps} />
      </div>
    </div>
  )
}
