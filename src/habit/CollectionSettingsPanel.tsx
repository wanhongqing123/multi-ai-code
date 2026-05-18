import { useEffect, useState } from 'react'
import {
  ALL_HABIT_EVENT_KINDS,
  ALLOWED_RETENTION_DAYS,
  HABIT_KIND_LABELS,
  type HabitEventKind,
  type HabitEventRow,
  type HabitSettings
} from './habitTypes'

interface Props {
  /** Live settings snapshot from main. */
  settings: HabitSettings
  /** Update one or more fields, returning the merged settings from main. */
  onUpdate: (patch: Partial<HabitSettings>) => Promise<void>
  /** Loaded recent events. */
  recent: HabitEventRow[]
  totalEventCount: number
  /** Refresh button. */
  onRefresh: () => Promise<void>
  /** Wipe all habit_events. */
  onClearEvents: () => Promise<void>
}

function fmtTs(ts: number): string {
  return new Date(ts).toLocaleString()
}

function truncate(text: string, max = 140): string {
  return text.length > max ? text.slice(0, max) + '…' : text
}

export default function CollectionSettingsPanel(props: Props): JSX.Element {
  const { settings, onUpdate, recent, totalEventCount, onRefresh, onClearEvents } = props
  const [working, setWorking] = useState(false)

  useEffect(() => {
    void onRefresh()
    // Refresh once when the panel mounts. Parent owns the data afterwards.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  async function toggleMaster() {
    setWorking(true)
    try {
      await onUpdate({ enabled: !settings.enabled })
    } finally {
      setWorking(false)
    }
  }

  async function toggleKind(kind: HabitEventKind) {
    const current = settings.kinds[kind] !== false
    setWorking(true)
    try {
      await onUpdate({ kinds: { [kind]: !current } })
    } finally {
      setWorking(false)
    }
  }

  async function setRetention(days: number) {
    setWorking(true)
    try {
      await onUpdate({ retentionDays: days })
    } finally {
      setWorking(false)
    }
  }

  async function handleClear() {
    if (!confirm('确定要清空所有已采集的行为数据吗？此操作不可撤销。')) return
    setWorking(true)
    try {
      await onClearEvents()
    } finally {
      setWorking(false)
    }
  }

  return (
    <div className="habit-settings-panel">
      <section className="habit-settings-section">
        <header className="habit-settings-section-head">
          <label className="habit-master-toggle">
            <input
              type="checkbox"
              checked={settings.enabled}
              disabled={working}
              onChange={() => void toggleMaster()}
            />
            <span>启用习惯采集</span>
          </label>
          <span className="habit-settings-hint">
            {settings.enabled
              ? '正在按下面勾选的事件类型记录到本地数据库'
              : '已停止采集，已有数据保留直至超过保留期或被清空'}
          </span>
        </header>

        <div className="habit-kind-toggles">
          {ALL_HABIT_EVENT_KINDS.map((kind) => {
            const checked = settings.kinds[kind] !== false
            return (
              <label key={kind} className="habit-kind-toggle">
                <input
                  type="checkbox"
                  checked={checked}
                  disabled={!settings.enabled || working}
                  onChange={() => void toggleKind(kind)}
                />
                <span className="habit-kind-toggle-label">{HABIT_KIND_LABELS[kind]}</span>
                <code className="habit-kind-toggle-code">{kind}</code>
              </label>
            )
          })}
        </div>
      </section>

      <section className="habit-settings-section">
        <header className="habit-settings-section-head">
          <strong>数据保留</strong>
          <span className="habit-settings-hint">超过保留期的事件在下次启动时自动清理</span>
        </header>
        <label className="habit-retention-select">
          保留期
          <select
            value={settings.retentionDays}
            disabled={working}
            onChange={(e) => void setRetention(Number(e.target.value))}
          >
            {ALLOWED_RETENTION_DAYS.map((d) => (
              <option key={d} value={d}>
                {d} 天
              </option>
            ))}
          </select>
        </label>
      </section>

      <section className="habit-settings-section">
        <header className="habit-settings-section-head">
          <strong>最近采集事件 · {totalEventCount}</strong>
          <span className="habit-settings-actions">
            <button
              type="button"
              className="drawer-btn"
              disabled={working}
              onClick={() => void onRefresh()}
            >
              刷新
            </button>
            <button
              type="button"
              className="drawer-btn warn"
              disabled={working || totalEventCount === 0}
              onClick={() => void handleClear()}
            >
              清空所有数据
            </button>
          </span>
        </header>

        {recent.length === 0 ? (
          <div className="drawer-empty">还没有采集到任何事件。</div>
        ) : (
          <ul className="habit-event-list">
            {recent.map((e) => {
              let text = ''
              try {
                text = (JSON.parse(e.payload) as { text?: string }).text ?? ''
              } catch {
                /* ignore */
              }
              return (
                <li key={e.id} className="habit-event-item">
                  <span className="habit-event-kind">
                    {HABIT_KIND_LABELS[e.kind as HabitEventKind] ?? e.kind}
                  </span>
                  <span className="habit-event-time">{fmtTs(e.ts)}</span>
                  <span className="habit-event-text">{truncate(text)}</span>
                </li>
              )
            })}
          </ul>
        )}
      </section>
    </div>
  )
}
