import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import type { Skill, SkillStep } from './skillTypes'
import { collectSkillVariables } from './skillTypes'

interface Props {
  /** Active main-session id, or null when idle. */
  sessionId: string | null
  /** Session running flag — disables execution when false. */
  sessionRunning: boolean
  /**
   * Refresh trigger from outside (e.g. when a candidate gets accepted in the
   * Skill Studio dialog). Increment this number to force a re-fetch.
   */
  refreshNonce?: number
  /** Open a small dialog to collect `{var}` values before running. */
  onNeedsVariables: (skill: Skill, vars: string[]) => void
  /** Execute a skill that needs no variable input. */
  onExecute: (skill: Skill) => Promise<void>
}

interface SkillSummary {
  id: number
  name: string
  description: string | null
  trigger: string | null
  steps: SkillStep[]
  lastUsedAt: number | null
}

function toSummary(raw: {
  id: number
  name: string
  description: string | null
  trigger: string | null
  steps: unknown[]
  lastUsedAt: number | null
}): SkillSummary {
  return {
    id: raw.id,
    name: raw.name,
    description: raw.description,
    trigger: raw.trigger,
    steps: (raw.steps as SkillStep[]) ?? [],
    lastUsedAt: raw.lastUsedAt
  }
}

/** Lightweight fuzzy match: every char of the query appears in order in the candidate. */
function looseMatch(query: string, haystack: string): boolean {
  if (!query) return true
  const q = query.toLowerCase()
  const h = haystack.toLowerCase()
  let qi = 0
  for (let i = 0; i < h.length && qi < q.length; i++) {
    if (h[i] === q[qi]) qi++
  }
  return qi === q.length
}

export default function SkillBar(props: Props): JSX.Element {
  const { sessionId, sessionRunning, refreshNonce, onNeedsVariables, onExecute } = props
  const [skills, setSkills] = useState<SkillSummary[]>([])
  const [query, setQuery] = useState('')
  const [focused, setFocused] = useState(false)
  const [activeIndex, setActiveIndex] = useState(0)
  const [running, setRunning] = useState(false)
  const [hint, setHint] = useState<string | null>(null)
  const containerRef = useRef<HTMLDivElement | null>(null)

  const refresh = useCallback(async () => {
    const list = await window.api.habit.skills.list()
    setSkills(list.map(toSummary))
  }, [])

  useEffect(() => {
    void refresh()
  }, [refresh, refreshNonce])

  // Close the dropdown when the user clicks outside of the bar entirely.
  useEffect(() => {
    function onDocMouseDown(e: MouseEvent) {
      if (!containerRef.current) return
      if (!containerRef.current.contains(e.target as Node)) setFocused(false)
    }
    document.addEventListener('mousedown', onDocMouseDown)
    return () => document.removeEventListener('mousedown', onDocMouseDown)
  }, [])

  // The user types `/` to open the menu; the rest of the input is treated as
  // the search query. So `/审` matches anything containing "审".
  const trimmedQuery = useMemo(() => {
    return query.startsWith('/') ? query.slice(1).trim() : query.trim()
  }, [query])

  const filtered = useMemo(() => {
    if (skills.length === 0) return []
    if (!trimmedQuery) return skills.slice(0, 8)
    return skills
      .filter((s) => {
        const trigger = s.trigger ?? ''
        if (trigger && looseMatch(trimmedQuery, trigger)) return true
        if (looseMatch(trimmedQuery, s.name)) return true
        if (s.description && looseMatch(trimmedQuery, s.description)) return true
        return false
      })
      .slice(0, 12)
  }, [skills, trimmedQuery])

  useEffect(() => {
    setActiveIndex(0)
  }, [trimmedQuery])

  async function runSelected(skill: SkillSummary) {
    if (!sessionRunning || !sessionId) {
      setHint('会话未启动，无法执行 skill')
      return
    }
    const vars = collectSkillVariables(skill.steps)
    if (vars.length > 0) {
      onNeedsVariables(
        {
          id: skill.id,
          name: skill.name,
          description: skill.description,
          trigger: skill.trigger,
          steps: skill.steps,
          source: null,
          candidateId: null,
          enabled: true,
          createdAt: 0,
          updatedAt: 0,
          lastUsedAt: skill.lastUsedAt
        },
        vars
      )
      setQuery('')
      setFocused(false)
      return
    }
    setRunning(true)
    setHint('执行中…')
    try {
      await onExecute({
        id: skill.id,
        name: skill.name,
        description: skill.description,
        trigger: skill.trigger,
        steps: skill.steps,
        source: null,
        candidateId: null,
        enabled: true,
        createdAt: 0,
        updatedAt: 0,
        lastUsedAt: skill.lastUsedAt
      })
      setHint(`已运行 · ${skill.name}`)
      setTimeout(() => setHint(null), 1800)
      void refresh()
    } catch (err) {
      setHint(`执行失败：${(err as Error).message}`)
    } finally {
      setRunning(false)
      setQuery('')
      setFocused(false)
    }
  }

  function onKeyDown(e: React.KeyboardEvent<HTMLInputElement>) {
    if (e.key === 'Escape') {
      setFocused(false)
      ;(e.target as HTMLInputElement).blur()
      return
    }
    if (filtered.length === 0) return
    if (e.key === 'ArrowDown') {
      e.preventDefault()
      setActiveIndex((i) => Math.min(filtered.length - 1, i + 1))
    } else if (e.key === 'ArrowUp') {
      e.preventDefault()
      setActiveIndex((i) => Math.max(0, i - 1))
    } else if (e.key === 'Enter') {
      e.preventDefault()
      const target = filtered[activeIndex]
      if (target) void runSelected(target)
    }
  }

  const showDropdown = focused && filtered.length > 0

  return (
    <div className="skill-bar" ref={containerRef}>
      <span className="skill-bar-icon" aria-hidden>/</span>
      <input
        className="skill-bar-input"
        placeholder={
          skills.length === 0
            ? '还没有 skill — 可在习惯监控里采纳一个候选或手动新建'
            : '/ 触发 skill，或直接输入关键词搜索'
        }
        value={query}
        disabled={running}
        onChange={(e) => {
          setQuery(e.target.value)
          if (!focused) setFocused(true)
        }}
        onFocus={() => setFocused(true)}
        onKeyDown={onKeyDown}
      />
      {hint && <span className="skill-bar-hint">{hint}</span>}
      {showDropdown && (
        <ul className="skill-bar-dropdown" role="listbox">
          {filtered.map((s, i) => (
            <li
              key={s.id}
              role="option"
              aria-selected={i === activeIndex}
              className={`skill-bar-item ${i === activeIndex ? 'active' : ''}`}
              onMouseDown={(e) => {
                // Prevent the input from losing focus before click fires.
                e.preventDefault()
              }}
              onClick={() => void runSelected(s)}
            >
              {s.trigger && <span className="skill-bar-item-trigger">/{s.trigger}</span>}
              <span className="skill-bar-item-name">{s.name}</span>
              {s.description && (
                <span className="skill-bar-item-desc">{s.description}</span>
              )}
              <span className="skill-bar-item-meta">
                {s.steps.length} 步
                {s.lastUsedAt
                  ? ` · ${new Date(s.lastUsedAt).toLocaleDateString()}`
                  : ''}
              </span>
            </li>
          ))}
        </ul>
      )}
    </div>
  )
}
