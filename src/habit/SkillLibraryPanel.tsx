import { useCallback, useEffect, useMemo, useState } from 'react'
import type { LocalSkillPackage, LocalSkillSnapshot, LocalSkillSource } from './localSkillTypes'
import SkillMarkdownPreview from './SkillMarkdownPreview'

interface Props {
  /** Bump after any skill enable/disable mutation so downstream UI refreshes. */
  onChanged: () => void
}

type StatusFilter = 'all' | 'enabled' | 'disabled' | 'issue'

function emptySnapshot(): LocalSkillSnapshot {
  return {
    sources: [],
    skills: [],
    totals: { discovered: 0, enabled: 0, disabled: 0 },
    scannedAt: new Date(0).toISOString()
  }
}

function statusText(skill: LocalSkillPackage): string {
  if (skill.health !== 'ok') return '有问题'
  return skill.enabled ? '启用' : '禁用'
}

function shortPath(path: string): string {
  if (path.length <= 58) return path
  return `…${path.slice(-55)}`
}

export default function SkillLibraryPanel(props: Props): JSX.Element {
  const { onChanged } = props
  const [snapshot, setSnapshot] = useState<LocalSkillSnapshot>(() => emptySnapshot())
  const [selectedId, setSelectedId] = useState<string | null>(null)
  const [detailSkillId, setDetailSkillId] = useState<string | null>(null)
  const [query, setQuery] = useState('')
  const [statusFilter, setStatusFilter] = useState<StatusFilter>('all')
  const [sourceFilter, setSourceFilter] = useState<string>('all')
  const [busy, setBusy] = useState(false)
  const [message, setMessage] = useState<string | null>(null)

  const refresh = useCallback(async () => {
    setBusy(true)
    try {
      const next = await window.api.habit.localSkills.scan()
      setSnapshot(next as LocalSkillSnapshot)
      setSelectedId((current) => current ?? next.skills[0]?.id ?? null)
      setMessage(`已扫描到 ${next.totals.discovered} 个 Skill`)
    } finally {
      setBusy(false)
    }
  }, [])

  useEffect(() => {
    void refresh()
  }, [refresh])

  const sourcesById = useMemo(
    () => new Map(snapshot.sources.map((source) => [source.id, source])),
    [snapshot.sources]
  )

  const filteredSkills = useMemo(() => {
    const needle = query.trim().toLowerCase()
    return snapshot.skills.filter((skill) => {
      if (statusFilter === 'enabled' && !skill.enabled) return false
      if (statusFilter === 'disabled' && skill.enabled) return false
      if (statusFilter === 'issue' && skill.health === 'ok') return false
      if (sourceFilter !== 'all' && skill.sourceId !== sourceFilter) return false
      if (!needle) return true
      return [
        skill.name,
        skill.description ?? '',
        skill.sourceName,
        skill.sourcePath,
        skill.dir
      ]
        .join('\n')
        .toLowerCase()
        .includes(needle)
    })
  }, [query, snapshot.skills, sourceFilter, statusFilter])

  const selected = useMemo(
    () =>
      filteredSkills.find((skill) => skill.id === selectedId) ??
      snapshot.skills.find((skill) => skill.id === selectedId) ??
      filteredSkills[0] ??
      null,
    [filteredSkills, selectedId, snapshot.skills]
  )

  const detailSkill = useMemo(
    () => snapshot.skills.find((skill) => skill.id === detailSkillId) ?? null,
    [detailSkillId, snapshot.skills]
  )

  async function toggleSkill(skill: LocalSkillPackage): Promise<void> {
    setBusy(true)
    try {
      const result = await window.api.habit.localSkills.setEnabled(skill.id, !skill.enabled)
      if (result.snapshot) {
        setSnapshot(result.snapshot as LocalSkillSnapshot)
        setMessage(`${skill.name} 已${skill.enabled ? '禁用' : '启用'}`)
        onChanged()
      }
    } finally {
      setBusy(false)
    }
  }

  async function setAllSkillsEnabled(enabled: boolean): Promise<void> {
    const targets = snapshot.skills.filter((skill) => skill.enabled !== enabled)
    if (targets.length === 0) {
      setMessage(enabled ? '所有 Skill 已经启用' : '所有 Skill 已经禁用')
      return
    }
    setBusy(true)
    try {
      let nextSnapshot: LocalSkillSnapshot | null = null
      for (const skill of targets) {
        const result = await window.api.habit.localSkills.setEnabled(skill.id, enabled)
        if (result.snapshot) nextSnapshot = result.snapshot as LocalSkillSnapshot
      }
      if (nextSnapshot) setSnapshot(nextSnapshot)
      setMessage(enabled ? `已启用 ${targets.length} 个 Skill` : `已禁用 ${targets.length} 个 Skill`)
      onChanged()
    } finally {
      setBusy(false)
    }
  }

  function selectSource(source: LocalSkillSource): void {
    setSourceFilter(source.id)
    const first = snapshot.skills.find((skill) => skill.sourceId === source.id)
    setSelectedId(first?.id ?? null)
  }

  const frontmatterEntries = detailSkill ? Object.entries(detailSkill.frontmatter) : []

  return (
    <div className="skill-manager-center" aria-busy={busy}>
      <section className="skill-manager-toolbar">
        <input
          className="plan-name-input skill-manager-search"
          value={query}
          onChange={(event) => setQuery(event.target.value)}
          placeholder="搜索 Skill 名称 / 描述 / 来源路径..."
        />
        <select
          className="plan-name-input skill-manager-filter"
          value={statusFilter}
          onChange={(event) => setStatusFilter(event.target.value as StatusFilter)}
        >
          <option value="all">全部状态</option>
          <option value="enabled">已启用</option>
          <option value="disabled">已禁用</option>
          <option value="issue">有问题</option>
        </select>
      </section>

      <div className="skill-manager-summary">
        <span>已发现 {snapshot.totals.discovered} 个 Skill</span>
        <span>启用 {snapshot.totals.enabled}</span>
        <span>禁用 {snapshot.totals.disabled}</span>
        <span>
          上次扫描{' '}
          {snapshot.scannedAt.startsWith('1970')
            ? '尚未扫描'
            : new Date(snapshot.scannedAt).toLocaleString()}
        </span>
        {message && <strong>{message}</strong>}
      </div>

      <div className="skill-manager-grid">
        <aside className="skill-manager-sources">
          <div className="skill-manager-pane-head">
            <strong>Skill 来源</strong>
            <button
              type="button"
              className={`skill-manager-source-all ${sourceFilter === 'all' ? 'active' : ''}`}
              onClick={() => setSourceFilter('all')}
            >
              全部
            </button>
          </div>
          <div className="skill-manager-source-list">
            {snapshot.sources.map((source) => (
              <button
                key={source.id}
                type="button"
                className={`skill-manager-source-card ${sourceFilter === source.id ? 'active' : ''}`}
                onClick={() => selectSource(source)}
              >
                <span className="skill-manager-source-dot" />
                <span>
                  <strong>{source.name}</strong>
                  <small>{shortPath(source.path)}</small>
                </span>
                <em>{source.enabledCount}/{source.skillCount}</em>
              </button>
            ))}
            {snapshot.sources.length === 0 && (
              <div className="drawer-empty">还没有扫描到 Skill 来源。</div>
            )}
          </div>
        </aside>

        <section className="skill-manager-list">
          <div className="skill-manager-pane-head">
            <span>{filteredSkills.length} 个匹配</span>
            <span className="skill-manager-bulk-actions">
              <button
                type="button"
                className="drawer-btn"
                disabled={busy || snapshot.skills.length === 0}
                onClick={() => void setAllSkillsEnabled(true)}
              >
                全部启用
              </button>
              <button
                type="button"
                className="drawer-btn"
                disabled={busy || snapshot.skills.length === 0}
                onClick={() => void setAllSkillsEnabled(false)}
              >
                全部禁用
              </button>
            </span>
          </div>
          <div className="skill-manager-cards">
            {filteredSkills.map((skill) => (
              <button
                key={skill.id}
                type="button"
                className={`skill-manager-card ${selected?.id === skill.id ? 'active' : ''}`}
                onClick={() => setSelectedId(skill.id)}
                onContextMenu={(event) => {
                  event.preventDefault()
                  setSelectedId(skill.id)
                  setDetailSkillId(skill.id)
                }}
                title="右键查看详情"
              >
                <span className={`skill-manager-health ${skill.health}`} />
                <span className="skill-manager-card-main">
                  <strong>{skill.name}</strong>
                  <small>{skill.description || '没有 description，请查看 SKILL.md。'}</small>
                  <em>{skill.sourceName}</em>
                </span>
                <span className={`skill-manager-state ${skill.enabled ? 'enabled' : 'disabled'}`}>
                  {statusText(skill)}
                </span>
                <span
                  className={`skill-manager-switch ${skill.enabled ? 'on' : ''}`}
                  onClick={(event) => {
                    event.stopPropagation()
                    void toggleSkill(skill)
                  }}
                >
                  <i />
                </span>
              </button>
            ))}
            {filteredSkills.length === 0 && (
              <div className="drawer-empty">没有匹配的 Skill。请调整搜索或筛选条件。</div>
            )}
          </div>
        </section>
      </div>

      <div className="skill-manager-apply-bar">
        <span>
          Skill 启用/禁用配置会自动附加到后续通过 Multi-AI Code 发送给 AICLI 的每条消息中，不再单独发送“应用”指令。
        </span>
      </div>

      {detailSkill && (
        <div
          className="skill-manager-detail-popover"
          role="dialog"
          aria-label="Skill 详情"
          onClick={() => setDetailSkillId(null)}
        >
          <aside className="skill-manager-detail-card" onClick={(event) => event.stopPropagation()}>
            <div className="skill-manager-pane-head">
              <strong>Skill 详情</strong>
              <button
                type="button"
                className="modal-close"
                onClick={() => setDetailSkillId(null)}
              >
                ×
              </button>
            </div>
            <div className="skill-manager-detail-title">
              <strong>{detailSkill.name}</strong>
              <span className={`skill-manager-state ${detailSkill.enabled ? 'enabled' : 'disabled'}`}>
                {statusText(detailSkill)}
              </span>
            </div>
            {detailSkill.description && <p>{detailSkill.description}</p>}
            <dl className="skill-manager-meta">
              <dt>来源</dt>
              <dd>{sourcesById.get(detailSkill.sourceId)?.name ?? detailSkill.sourceName}</dd>
              <dt>路径</dt>
              <dd title={detailSkill.dir}>{shortPath(detailSkill.dir)}</dd>
              <dt>文件</dt>
              <dd title={detailSkill.skillFile}>{shortPath(detailSkill.skillFile)}</dd>
              <dt>版本</dt>
              <dd>{detailSkill.version || '未声明'}</dd>
            </dl>
            <div className="skill-manager-frontmatter">
              <strong>frontmatter</strong>
              {frontmatterEntries.length === 0 ? (
                <span>没有 frontmatter</span>
              ) : (
                <pre>
                  {frontmatterEntries
                    .map(([key, value]) => `${key}: ${value}`)
                    .join('\n')}
                </pre>
              )}
            </div>
            <div className="skill-manager-preview">
              <strong>SKILL.md 预览</strong>
              <SkillMarkdownPreview markdown={detailSkill.markdown || detailSkill.preview} />
            </div>
          </aside>
        </div>
      )}
    </div>
  )
}
