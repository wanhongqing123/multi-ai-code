import { useMemo } from 'react'
import { diffLines } from 'diff'

export interface DiffViewProps {
  oldText: string
  newText: string
  oldLabel?: string
  newLabel?: string
}

export default function DiffView({ oldText, newText, oldLabel = '旧版', newLabel = '新版' }: DiffViewProps) {
  const parts = useMemo(() => diffLines(oldText, newText), [oldText, newText])
  return (
    <div className="diff-view">
      <div className="diff-head">
        <span className="diff-head-old">── {oldLabel}</span>
        <span className="diff-head-new">{newLabel} ──</span>
      </div>
      <pre className="diff-body">
        {parts.map((p, i) => {
          const cls = p.added ? 'diff-add' : p.removed ? 'diff-del' : 'diff-same'
          const prefix = p.added ? '+ ' : p.removed ? '- ' : '  '
          const lines = p.value.replace(/\n$/, '').split('\n')
          return (
            <span key={i} className={cls}>
              {lines.map((ln, j) => (
                <span key={j} className="diff-line">
                  {prefix}
                  {ln}
                  {'\n'}
                </span>
              ))}
            </span>
          )
        })}
      </pre>
    </div>
  )
}
