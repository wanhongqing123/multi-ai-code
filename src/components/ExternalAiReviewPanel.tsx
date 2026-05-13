import { useEffect, useRef, useState } from 'react'
import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'
import type {
  ExternalReviewAssessmentItem,
  ExternalReviewDecisionPayload,
  ExternalReviewSuggestion
} from './externalAiReview.js'

function decisionLabel(value: ExternalReviewDecisionPayload['decision']): string {
  if (value === 'accepted') return '采纳'
  if (value === 'rejected') return '不采纳'
  return '需人工确认'
}

function compactLocation(item: ExternalReviewAssessmentItem): string {
  return [item.fileHint?.trim(), item.lineHint?.trim()].filter(Boolean).join(':') || '-'
}

function renderAssessmentTable(
  title: string,
  items: ExternalReviewAssessmentItem[] | undefined
): JSX.Element | null {
  if (!items || items.length === 0) return null
  return (
    <section className="dv-external-review-result-section">
      <h5>{title}</h5>
      <table className="dv-external-review-result-table">
        <thead>
          <tr>
            <th>问题</th>
            <th>位置</th>
            <th>理由</th>
            <th>建议</th>
          </tr>
        </thead>
        <tbody>
          {items.map((item, index) => (
            <tr key={`${title}-${index}-${item.title}`}>
              <td>{item.title}</td>
              <td>{compactLocation(item)}</td>
              <td>{item.reason}</td>
              <td>{item.recommendation?.trim() || '-'}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </section>
  )
}

function renderDecisionResult(suggestion: ExternalReviewSuggestion): JSX.Element | null {
  if (!suggestion.decisionReason) return null
  const payload = suggestion.decisionPayload
  if (!payload) {
    return (
      <div className="dv-external-review-item-reason">
        <ReactMarkdown remarkPlugins={[remarkGfm]}>{suggestion.decisionReason}</ReactMarkdown>
      </div>
    )
  }
  const hasStructuredSections =
    (payload.acceptedChanges?.length ?? 0) > 0 ||
    (payload.rejectedChanges?.length ?? 0) > 0 ||
    (payload.modificationPlan?.length ?? 0) > 0
  if (!hasStructuredSections) {
    return (
      <div className="dv-external-review-item-reason">
        <ReactMarkdown remarkPlugins={[remarkGfm]}>{suggestion.decisionReason}</ReactMarkdown>
      </div>
    )
  }

  return (
    <div className="dv-external-review-result">
      <div className="dv-external-review-result-summary">
        <strong>结论：</strong>
        <span>{decisionLabel(payload.decision)}</span>
        <span className="dv-external-review-result-dot">·</span>
        <span>{payload.reason}</span>
      </div>
      {renderAssessmentTable('需修改（建议采纳）', payload.acceptedChanges)}
      {renderAssessmentTable('无需修改（不建议采纳）', payload.rejectedChanges)}
      {payload.modificationPlan && payload.modificationPlan.length > 0 ? (
        <section className="dv-external-review-result-section">
          <h5>建议修改方案</h5>
          <ol className="dv-external-review-plan-list">
            {payload.modificationPlan.map((step, index) => (
              <li key={`plan-${index}`}>{step}</li>
            ))}
          </ol>
        </section>
      ) : null}
    </div>
  )
}

export default function ExternalAiReviewPanel(props: {
  sourceLabel: string
  sourcePath?: string
  suggestions: ExternalReviewSuggestion[]
  busy: boolean
  autoOpenToken?: number
  onImport: () => void
  onJudgeOne: (id: string) => void
  onJudgeAll: () => Promise<void> | void
}) {
  const [previewOpen, setPreviewOpen] = useState(false)
  const [judgeAllPending, setJudgeAllPending] = useState(false)
  const lastAutoOpenToken = useRef<number | null>(null)
  const judgeAllLockRef = useRef(false)
  const mountedRef = useRef(true)
  const hasReviewContent = props.suggestions.some(
    (item) => item.rawText.trim().length > 0
  )
  const hasIdleSuggestion = props.suggestions.some((item) => item.status === 'idle')

  useEffect(() => {
    mountedRef.current = true
    return () => {
      mountedRef.current = false
    }
  }, [])

  const handleJudgeAll = () => {
    if (
      judgeAllLockRef.current ||
      judgeAllPending ||
      props.busy ||
      !hasReviewContent ||
      !hasIdleSuggestion
    ) {
      return
    }
    judgeAllLockRef.current = true
    setJudgeAllPending(true)
    void Promise.resolve(props.onJudgeAll()).finally(() => {
      judgeAllLockRef.current = false
      if (mountedRef.current) {
        setJudgeAllPending(false)
      }
    })
  }

  useEffect(() => {
    if ((props.autoOpenToken ?? 0) <= 0) return
    if (props.suggestions.length === 0) return
    if (lastAutoOpenToken.current === props.autoOpenToken) return
    lastAutoOpenToken.current = props.autoOpenToken ?? null
    setPreviewOpen(true)
  }, [props.autoOpenToken, props.suggestions.length])

  return (
    <section className="dv-external-review">
      <div className="dv-external-review-head">
        <span className="dv-external-review-title">外部 AI 建议</span>
        <div className="dv-external-review-actions">
          <button
            className="dv-inline-ann-action"
            onClick={props.onImport}
            disabled={props.busy}
          >
            导入
          </button>
          <button
            className="dv-inline-ann-action"
            onClick={handleJudgeAll}
            disabled={
              props.busy ||
              judgeAllPending ||
              !hasReviewContent ||
              !hasIdleSuggestion
            }
          >
            判断
          </button>
          <button
            className="dv-inline-ann-action"
            onClick={() => setPreviewOpen(true)}
            disabled={!hasReviewContent}
            title="查看完整内容"
          >
            查看
          </button>
        </div>
      </div>
      <div
        className="dv-external-review-source"
        title={props.sourcePath || props.sourceLabel || ''}
      >
        {props.sourceLabel || '尚未导入外部 review'}
      </div>
      {props.suggestions.length === 0 ? (
        <div className="dv-external-review-empty">
          导入后会在这里展示完整的 Markdown review 内容
        </div>
      ) : (
        <div className="dv-external-review-content">
          {props.suggestions.map((suggestion) => (
            <article
              key={suggestion.id}
              className={`dv-external-review-item status-${suggestion.status}`}
            >
              <div className="dv-external-review-markdown">
                <ReactMarkdown remarkPlugins={[remarkGfm]}>
                  {suggestion.rawText}
                </ReactMarkdown>
              </div>
              {props.suggestions.length > 1 ? (
                <div className="dv-external-review-item-actions">
                  <button
                    className="dv-inline-ann-action"
                    onClick={() => props.onJudgeOne(suggestion.id)}
                    disabled={props.busy || suggestion.status !== 'idle'}
                  >
                    判断此条
                  </button>
                </div>
              ) : null}
              {renderDecisionResult(suggestion)}
            </article>
          ))}
        </div>
      )}

      {previewOpen && (
        <div
          className="dv-external-review-preview-backdrop"
          onClick={() => setPreviewOpen(false)}
        >
          <div
            className="dv-external-review-preview"
            onClick={(event) => event.stopPropagation()}
          >
            <div className="dv-external-review-preview-head">
              <span>外部 Review 预览</span>
              <button
                className="dv-inline-ann-action"
                onClick={() => setPreviewOpen(false)}
              >
                关闭
              </button>
            </div>
            <div className="dv-external-review-preview-body">
              {props.suggestions.map((suggestion) => (
                <article key={`preview-${suggestion.id}`} className="dv-external-review-item">
                  <div className="dv-external-review-markdown">
                    <ReactMarkdown remarkPlugins={[remarkGfm]}>
                      {suggestion.rawText}
                    </ReactMarkdown>
                  </div>
                  {renderDecisionResult(suggestion)}
                </article>
              ))}
            </div>
          </div>
        </div>
      )}
    </section>
  )
}
