import { useEffect, useRef, useState } from 'react'

export interface GlobalSearchDialogProps {
  projectId: string
  projectDir: string
  onClose: () => void
}

interface Hit {
  source: 'artifact' | 'template'
  title: string
  snippet: string
  location: string
  open?: () => void
}

function highlight(text: string, q: string): JSX.Element[] {
  if (!q) return [<span key="x">{text}</span>]
  const lower = text.toLowerCase()
  const lo = q.toLowerCase()
  const parts: JSX.Element[] = []
  let i = 0
  let k = 0
  while (true) {
    const idx = lower.indexOf(lo, i)
    if (idx === -1) {
      parts.push(<span key={k++}>{text.slice(i)}</span>)
      break
    }
    if (idx > i) parts.push(<span key={k++}>{text.slice(i, idx)}</span>)
    parts.push(<mark key={k++}>{text.slice(idx, idx + q.length)}</mark>)
    i = idx + q.length
  }
  return parts
}

export default function GlobalSearchDialog({ projectId, projectDir, onClose }: GlobalSearchDialogProps) {
  const [q, setQ] = useState('')
  const [results, setResults] = useState<Hit[]>([])
  const [searching, setSearching] = useState(false)
  const inputRef = useRef<HTMLInputElement | null>(null)

  useEffect(() => {
    inputRef.current?.focus()
  }, [])

  useEffect(() => {
    const id = setTimeout(async () => {
      if (!q.trim()) {
        setResults([])
        return
      }
      setSearching(true)
      const hits: Hit[] = []
      // Artifacts
      const res = await window.api.search.artifacts(projectId, q)
      if (res.ok) {
        for (const r of res.results) {
          hits.push({
            source: 'artifact',
            title: r.path,
            snippet: r.snippet,
            location: `Stage ${r.stageId} · 行 ${r.line}`
          })
        }
      }
      // Templates (localStorage)
      try {
        const raw = localStorage.getItem('multi-ai-code.templates')
        if (raw) {
          const list = JSON.parse(raw) as { id: string; name: string; body: string }[]
          for (const t of list) {
            if ((t.name + '\n' + t.body).toLowerCase().includes(q.toLowerCase())) {
              hits.push({
                source: 'template',
                title: t.name,
                snippet: t.body.split('\n').find((ln) => ln.toLowerCase().includes(q.toLowerCase())) ?? t.body.slice(0, 200),
                location: '模板'
              })
            }
          }
        }
      } catch {
        /* ignore */
      }
      setResults(hits)
      setSearching(false)
    }, 220)
    return () => clearTimeout(id)
  }, [q, projectId])

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal global-search-modal" onClick={(e) => e.stopPropagation()}>
        <input
          ref={inputRef}
          className="gs-input"
          placeholder="全局搜索：产物归档 + 模板（支持跨阶段 · 防抖 220ms）"
          value={q}
          onChange={(e) => setQ(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === 'Escape') onClose()
          }}
        />
        <div className="gs-results">
          {searching ? (
            <div className="drawer-empty">搜索中…</div>
          ) : q && results.length === 0 ? (
            <div className="drawer-empty">无匹配结果</div>
          ) : !q ? (
            <div className="drawer-empty">输入关键词开始搜索</div>
          ) : (
            results.map((h, i) => (
              <div key={i} className="gs-hit">
                <div className="gs-hit-head">
                  <span className={`gs-hit-tag gs-tag-${h.source}`}>
                    {h.source === 'artifact' ? '📦 产物' : '📋 模板'}
                  </span>
                  <span className="gs-hit-title" title={h.title}>
                    {highlight(h.title, q)}
                  </span>
                  <span className="gs-hit-loc">{h.location}</span>
                </div>
                <div className="gs-hit-snippet">{highlight(h.snippet, q)}</div>
              </div>
            ))
          )}
          <div style={{ opacity: 0.5, fontSize: 10, padding: 8 }}>
            项目：<code>{projectDir}</code>
          </div>
        </div>
      </div>
    </div>
  )
}
