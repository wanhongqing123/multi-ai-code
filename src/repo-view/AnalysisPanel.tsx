import { useState } from 'react'
import {
  canSendRepoAnnotations,
  dispatchRepoSendQuestion,
  repoSendButtonTitle
} from './analysisPanelState'

export interface RepoCodeAnnotation {
  id: string
  filePath: string
  lineRange: string
  snippet: string
  comment: string
}

export default function AnalysisPanel({
  filePath,
  annotations,
  activeAnnotationId = null,
  recentlyAddedAnnotationId = null,
  sessionRunning,
  sending,
  onSendToCli,
  onEditAnnotation,
  onRemoveAnnotation,
  onClearAnnotations
}: {
  filePath: string
  annotations: RepoCodeAnnotation[]
  activeAnnotationId?: string | null
  recentlyAddedAnnotationId?: string | null
  sessionRunning: boolean
  sending: boolean
  onSendToCli: (question: string) => Promise<boolean>
  onEditAnnotation: (id: string) => void
  onRemoveAnnotation: (id: string) => void
  onClearAnnotations: () => void
}): JSX.Element {
  const [question, setQuestion] = useState('')
  const canSend = canSendRepoAnnotations(sessionRunning, annotations.length, sending)

  return (
    <div className="repo-analysis-panel">
      <div className="repo-analysis-head">代码标注</div>
      {!filePath && annotations.length === 0 ? (
        <div className="repo-analysis-empty">先从左侧选择一个文件。</div>
      ) : (
        <>
          <div className="repo-analysis-subhead">待发送标注（{annotations.length}）</div>
          {annotations.length === 0 ? (
            <div className="repo-analysis-empty">
              在代码区选中文本后点击“标注”，即可加入待发送标注列表。
            </div>
          ) : (
            <ul className="repo-analysis-list">
              {annotations.map((a, i) => {
                const itemClassName = [
                  'repo-analysis-item',
                  activeAnnotationId === a.id ? 'active' : '',
                  recentlyAddedAnnotationId === a.id ? 'recent' : ''
                ]
                  .filter(Boolean)
                  .join(' ')

                return (
                  <li key={a.id} className={itemClassName}>
                    <div className="repo-analysis-item-head">
                      <span>#{i + 1}</span>
                      <span title={a.filePath}>{a.filePath}</span>
                      <span>{a.lineRange} 行</span>
                      <div className="repo-analysis-item-actions">
                        <button
                          className="repo-analysis-edit"
                          onClick={() => onEditAnnotation(a.id)}
                        >
                          编辑
                        </button>
                        <button
                          className="repo-analysis-remove"
                          onClick={() => onRemoveAnnotation(a.id)}
                        >
                          删除
                        </button>
                      </div>
                    </div>
                    <blockquote className="repo-analysis-quote">
                      {a.snippet.length > 800
                        ? `${a.snippet.slice(0, 800)}\n…`
                        : a.snippet}
                    </blockquote>
                    <div className="repo-analysis-comment">{a.comment}</div>
                  </li>
                )
              })}
            </ul>
          )}
          <label className="repo-analysis-input-label">问题（可选）</label>
          <textarea
            className="repo-analysis-input"
            value={question}
            onChange={(e) => setQuestion(e.target.value)}
            rows={4}
            placeholder="例如：这些代码的主流程、边界条件和潜在风险是什么？"
          />
          <div className="repo-analysis-actions">
            <button
              className="drawer-btn"
              onClick={onClearAnnotations}
              disabled={annotations.length === 0}
            >
              清除
            </button>
            <button
              className="drawer-btn primary"
              disabled={!canSend}
              onClick={async () => {
                const shouldClear = await dispatchRepoSendQuestion(question, onSendToCli)
                if (shouldClear) setQuestion('')
              }}
              title={repoSendButtonTitle(sessionRunning, annotations.length, sending)}
            >
              {sending ? '发送中...' : '发送'}
            </button>
          </div>
        </>
      )}
    </div>
  )
}
