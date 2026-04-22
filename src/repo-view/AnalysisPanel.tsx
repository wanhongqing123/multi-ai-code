import { useMemo, useState } from 'react'
import { getCliTargetLabel, type AiCliKind } from '../components/cliTarget'

export interface RepoCodeAnnotation {
  id: string
  filePath: string
  lineRange: string
  snippet: string
}

export default function AnalysisPanel({
  filePath,
  annotations,
  aiCli,
  running,
  answer,
  recentTopics,
  onSendAnalysis,
  onRemoveAnnotation,
  onClearAnnotations
}: {
  projectId: string
  repoRoot: string
  filePath: string
  annotations: RepoCodeAnnotation[]
  aiCli: AiCliKind
  running: boolean
  answer: string
  recentTopics: Array<{ at: string; filePath: string; topic: string }>
  onSendAnalysis: (question: string) => void
  onRemoveAnnotation: (id: string) => void
  onClearAnnotations: () => void
}): JSX.Element {
  const [question, setQuestion] = useState('')
  const targetLabel = useMemo(() => getCliTargetLabel(aiCli), [aiCli])
  const canSend = annotations.length > 0 && !running

  return (
    <div className="repo-analysis-panel">
      <div className="repo-analysis-head">代码分析</div>
      {!filePath ? (
        <div className="repo-analysis-empty">先从左侧选择一个文件。</div>
      ) : (
        <>
          <div className="repo-analysis-subhead">已标注片段（{annotations.length}）</div>
          {annotations.length === 0 ? (
            <div className="repo-analysis-empty">
              在代码区选中文本后点击“✏ 标注”，即可把片段加入分析队列。
            </div>
          ) : (
            <ul className="repo-analysis-list">
              {annotations.map((a, i) => (
                <li key={a.id} className="repo-analysis-item">
                  <div className="repo-analysis-item-head">
                    <span>#{i + 1}</span>
                    <span>{a.lineRange} 行</span>
                    <button
                      className="repo-analysis-remove"
                      onClick={() => onRemoveAnnotation(a.id)}
                    >
                      删除
                    </button>
                  </div>
                  <blockquote className="repo-analysis-quote">
                    {a.snippet.length > 260 ? `${a.snippet.slice(0, 260)}…` : a.snippet}
                  </blockquote>
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
              onClick={() => onSendAnalysis(question.trim())}
              title={
                canSend
                  ? `发送到独立 ${targetLabel} 进行分析`
                  : running
                  ? '分析进行中，请等待本轮完成'
                  : '至少需要一条标注'
              }
            >
              {running ? '分析中…' : `发送给 ${targetLabel}`}
            </button>
          </div>
          {answer && <pre className="repo-analysis-answer">{answer}</pre>}
          {recentTopics.length > 0 && (
            <div className="repo-analysis-recent">
              <div className="repo-analysis-subhead">最近主题</div>
              <ul className="repo-analysis-recent-list">
                {recentTopics.slice(0, 8).map((x, i) => (
                  <li key={`${x.at}_${i}`}>
                    {x.topic} · {x.filePath}
                  </li>
                ))}
              </ul>
            </div>
          )}
        </>
      )}
    </div>
  )
}
