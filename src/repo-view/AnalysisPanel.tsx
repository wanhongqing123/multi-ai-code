import { useState } from 'react'

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
  onSendToCli,
  onEditAnnotation,
  onRemoveAnnotation,
  onClearAnnotations
}: {
  filePath: string
  annotations: RepoCodeAnnotation[]
  onSendToCli: (question: string) => void
  onEditAnnotation: (id: string) => void
  onRemoveAnnotation: (id: string) => void
  onClearAnnotations: () => void
}): JSX.Element {
  const [question, setQuestion] = useState('')
  const canSend = annotations.length > 0

  return (
    <div className="repo-analysis-panel">
      <div className="repo-analysis-head">代码标注</div>
      {!filePath ? (
        <div className="repo-analysis-empty">先从左侧选择一个文件。</div>
      ) : (
        <>
          <div className="repo-analysis-subhead">已标注片段（{annotations.length}）</div>
          {annotations.length === 0 ? (
            <div className="repo-analysis-empty">
              在代码区选中文本后点击"✏ 标注"，即可把片段加入分析队列。
            </div>
          ) : (
            <ul className="repo-analysis-list">
              {annotations.map((a, i) => (
                <li key={a.id} className="repo-analysis-item">
                  <div className="repo-analysis-item-head">
                    <span>#{i + 1}</span>
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
                    {a.snippet.length > 260 ? `${a.snippet.slice(0, 260)}…` : a.snippet}
                  </blockquote>
                  <div className="repo-analysis-comment">{a.comment}</div>
                </li>
              ))}
            </ul>
          )}
          <label className="repo-analysis-input-label">问题（可选）</label>
          <textarea
            className="repo-analysis-input"
            value={question}
            onChange={(e) => setQuestion(e.target.value)}
            rows={4}
            placeholder="例如：这段代码的主流程、边界条件和潜在风险是什么？"
          />
          <div className="repo-analysis-actions">
            <button
              className="drawer-btn"
              onClick={onClearAnnotations}
              disabled={annotations.length === 0}
            >
              清空标注
            </button>
            <button
              className="drawer-btn primary"
              disabled={!canSend}
              onClick={() => {
                onSendToCli(question.trim())
                setQuestion('')
              }}
              title={canSend ? '注入到下方 AI CLI' : '至少需要一条标注'}
            >
              发送到 AI CLI
            </button>
          </div>
        </>
      )}
    </div>
  )
}
