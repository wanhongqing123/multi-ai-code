import { useCallback, useEffect, useState } from 'react'
import CollectionSettingsPanel from './CollectionSettingsPanel'
import SkillCandidatesPanel from './SkillCandidatesPanel'
import SkillLibraryPanel from './SkillLibraryPanel'
import type {
  HabitEventRow,
  HabitSettings,
  SkillCandidateRow
} from './habitTypes'
import type { Skill, SkillStep } from './skillTypes'

interface Props {
  onClose: () => void
  /** Opens the AI settings dialog so the user can verify the source CLI config. */
  onOpenAiSettings: () => void
  /** Current main-session CLI label for the source hint. */
  mainCliLabel: string
  /** True when a main-session is alive — gates Skill Library "试运行". */
  sessionRunning: boolean
  /** Hook to actually run a skill (the parent owns the runtime deps). */
  onRunSkill: (skill: Skill) => void
  /** Bump after any skill list mutation so SkillBar refreshes. */
  onSkillsChanged: () => void
}

type Tab = 'candidates' | 'library' | 'collection'

export default function SkillStudioDialog(props: Props): JSX.Element {
  const {
    onClose,
    onOpenAiSettings,
    mainCliLabel,
    sessionRunning,
    onRunSkill,
    onSkillsChanged
  } = props
  const [tab, setTab] = useState<Tab>('candidates')

  const [settings, setSettings] = useState<HabitSettings | null>(null)
  const [recent, setRecent] = useState<HabitEventRow[]>([])
  const [totalEventCount, setTotalEventCount] = useState(0)
  const [candidates, setCandidates] = useState<SkillCandidateRow[]>([])
  const [analysisRunning, setAnalysisRunning] = useState(false)
  const [toast, setToast] = useState<string | null>(null)

  function flashToast(msg: string): void {
    setToast(msg)
    setTimeout(() => setToast(null), 2500)
  }

  useEffect(() => {
    void (async () => {
      const s = (await window.api.habit.settings.get()) as HabitSettings
      setSettings(s)
    })()
  }, [])

  const refreshEvents = useCallback(async () => {
    const res = await window.api.habit.events.recent(100)
    setRecent(res.events as HabitEventRow[])
    setTotalEventCount(res.total)
  }, [])

  const refreshCandidates = useCallback(async () => {
    const list = (await window.api.habit.candidates.list({
      statuses: ['pending', 'snoozed', 'error']
    })) as SkillCandidateRow[]
    setCandidates(list)
  }, [])

  const handleUpdateSettings = useCallback(async (patch: Partial<HabitSettings>) => {
    const next = (await window.api.habit.settings.update(patch)) as HabitSettings
    setSettings(next)
  }, [])

  const handleClearEvents = useCallback(async () => {
    const res = await window.api.habit.events.clear()
    flashToast(`已清空 ${res.removed} 条事件`)
    await refreshEvents()
  }, [refreshEvents])

  const handleAcceptCandidate = useCallback(
    async (
      c: SkillCandidateRow,
      payload: {
        name: string
        description?: string | null
        trigger?: string | null
        steps: SkillStep[]
      }
    ) => {
      const res = await window.api.habit.candidates.acceptAsSkill({
        candidateId: c.id,
        ...payload
      })
      if (res.ok) {
        flashToast(`已采纳为 skill #${res.id}`)
        onSkillsChanged()
        await refreshCandidates()
      } else {
        flashToast('采纳失败')
      }
    },
    [refreshCandidates, onSkillsChanged]
  )

  const handleDiscardCandidate = useCallback(
    async (id: number) => {
      await window.api.habit.candidates.updateStatus({ id, status: 'discarded' })
      flashToast('已丢弃')
      await refreshCandidates()
    },
    [refreshCandidates]
  )

  const handleSnoozeCandidate = useCallback(
    async (id: number) => {
      const snoozedUntil = Date.now() + 7 * 24 * 60 * 60 * 1000
      await window.api.habit.candidates.updateStatus({
        id,
        status: 'snoozed',
        snoozedUntil
      })
      flashToast('已暂不处理（7 天后重新出现）')
      await refreshCandidates()
    },
    [refreshCandidates]
  )

  const handleRunAnalysisNow = useCallback(async () => {
    if (analysisRunning) return
    setAnalysisRunning(true)
    try {
      const res = await window.api.habit.runNow()
      if (!res.ok) {
        flashToast(`分析失败：${res.error}`)
        return
      }
      const o = res.outcome
      if (o.reason === 'completed') {
        flashToast(
          `分析完成：${o.clustersFound ?? 0} 个簇，新增 ${o.candidatesInserted ?? 0} 条候选`
        )
        await refreshCandidates()
      } else if (o.reason === 'no-events') {
        flashToast('暂无可分析事件')
      } else if (o.reason === 'no-clusters') {
        flashToast('未发现可聚类的重复模式')
      } else if (o.reason === 'disabled') {
        flashToast('采集已关闭，请先在采集 tab 启用')
      } else {
        flashToast('已跳过本次分析')
      }
    } finally {
      setAnalysisRunning(false)
    }
  }, [analysisRunning, refreshCandidates])

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal templates-modal" onClick={(e) => e.stopPropagation()}>
        <div className="modal-head">
          <h3>🎓 Skill 学习</h3>
          <button className="modal-close" onClick={onClose}>
            ×
          </button>
        </div>

        <div className="habit-source-hint">
          习惯学习智能体当前使用：<strong>{mainCliLabel}</strong>（与主会话 AI 同一份配置）。{' '}
          <button type="button" className="habit-source-link" onClick={onOpenAiSettings}>
            打开设置
          </button>
        </div>

        <div className="habit-tabs">
          <button
            type="button"
            className={`habit-tab-btn ${tab === 'candidates' ? 'active' : ''}`}
            onClick={() => setTab('candidates')}
          >
            候选
          </button>
          <button
            type="button"
            className={`habit-tab-btn ${tab === 'library' ? 'active' : ''}`}
            onClick={() => setTab('library')}
          >
            Skill 库
          </button>
          <button
            type="button"
            className={`habit-tab-btn ${tab === 'collection' ? 'active' : ''}`}
            onClick={() => setTab('collection')}
          >
            采集
          </button>
        </div>

        <div className="habit-tab-body">
          {tab === 'candidates' && (
            <SkillCandidatesPanel
              candidates={candidates}
              onRefresh={refreshCandidates}
              onAccept={handleAcceptCandidate}
              onDiscard={handleDiscardCandidate}
              onSnooze={handleSnoozeCandidate}
              onRunAnalysisNow={handleRunAnalysisNow}
              analysisRunning={analysisRunning}
            />
          )}
          {tab === 'library' && (
            <SkillLibraryPanel
              sessionRunning={sessionRunning}
              onTestRun={onRunSkill}
              onChanged={onSkillsChanged}
            />
          )}
          {tab === 'collection' && settings && (
            <CollectionSettingsPanel
              settings={settings}
              onUpdate={handleUpdateSettings}
              recent={recent}
              totalEventCount={totalEventCount}
              onRefresh={refreshEvents}
              onClearEvents={handleClearEvents}
            />
          )}
          {tab === 'collection' && !settings && (
            <div className="drawer-empty">加载设置中…</div>
          )}
        </div>

        {toast && <div className="habit-toast">{toast}</div>}
      </div>
    </div>
  )
}
