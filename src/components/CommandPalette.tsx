import { useEffect, useMemo, useRef, useState } from 'react'

export interface Command {
  id: string
  label: string
  keywords?: string
  hint?: string
  action: () => void | Promise<void>
  disabled?: boolean
}

export interface CommandPaletteProps {
  commands: Command[]
  onClose: () => void
}

export default function CommandPalette({ commands, onClose }: CommandPaletteProps) {
  const [q, setQ] = useState('')
  const [idx, setIdx] = useState(0)
  const inputRef = useRef<HTMLInputElement | null>(null)

  useEffect(() => {
    inputRef.current?.focus()
  }, [])

  const filtered = useMemo(() => {
    const query = q.trim().toLowerCase()
    const active = commands.filter((c) => !c.disabled)
    if (!query) return active
    return active
      .map((c) => {
        const hay = `${c.label} ${c.keywords ?? ''}`.toLowerCase()
        let score = 0
        let j = 0
        for (const ch of query) {
          const k = hay.indexOf(ch, j)
          if (k === -1) return null
          score += k - j
          j = k + 1
        }
        return { c, score }
      })
      .filter((x): x is { c: Command; score: number } => !!x)
      .sort((a, b) => a.score - b.score)
      .map((x) => x.c)
  }, [q, commands])

  useEffect(() => setIdx(0), [q])

  async function run(c: Command) {
    onClose()
    await c.action()
  }

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div
        className="cmdk-modal"
        onClick={(e) => e.stopPropagation()}
        onKeyDown={(e) => {
          if (e.key === 'ArrowDown') {
            e.preventDefault()
            setIdx((i) => Math.min(i + 1, filtered.length - 1))
          } else if (e.key === 'ArrowUp') {
            e.preventDefault()
            setIdx((i) => Math.max(i - 1, 0))
          } else if (e.key === 'Enter') {
            const c = filtered[idx]
            if (c) void run(c)
          } else if (e.key === 'Escape') {
            onClose()
          }
        }}
      >
        <input
          ref={inputRef}
          className="cmdk-input"
          placeholder="输入命令、或搜索功能…（↑↓ 移动，Enter 执行，Esc 关闭）"
          value={q}
          onChange={(e) => setQ(e.target.value)}
        />
        <div className="cmdk-list">
          {filtered.length === 0 ? (
            <div className="drawer-empty">无匹配项</div>
          ) : (
            filtered.map((c, i) => (
              <div
                key={c.id}
                className={`cmdk-item ${i === idx ? 'active' : ''}`}
                onMouseEnter={() => setIdx(i)}
                onClick={() => void run(c)}
              >
                <span className="cmdk-label">{c.label}</span>
                {c.hint && <span className="cmdk-hint">{c.hint}</span>}
              </div>
            ))
          )}
        </div>
      </div>
    </div>
  )
}
