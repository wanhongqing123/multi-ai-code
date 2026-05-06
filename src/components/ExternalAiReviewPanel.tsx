import type { ExternalReviewSuggestion } from './externalAiReview.js'

export default function ExternalAiReviewPanel(props: {
  sourceLabel: string
  suggestions: ExternalReviewSuggestion[]
  busy: boolean
  onImport: () => void
  onJudgeOne: (id: string) => void
  onJudgeAll: () => void
}) {
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
            onClick={props.onJudgeAll}
            disabled={props.busy || props.suggestions.length === 0}
          >
            全部判断
          </button>
        </div>
      </div>
      <div className="dv-external-review-source">
        {props.sourceLabel || '尚未导入外部 review'}
      </div>
      {props.suggestions.length === 0 ? (
        <div className="dv-external-review-empty">导入后会在这里显示逐条建议</div>
      ) : (
        <ul className="dv-external-review-list">
          {props.suggestions.map((suggestion) => (
            <li
              key={suggestion.id}
              className={`dv-external-review-item status-${suggestion.status}`}
            >
              <div className="dv-external-review-item-text">{suggestion.rawText}</div>
              <div className="dv-external-review-item-meta">
                <span>
                  {suggestion.linkedDiffFile?.path ??
                    suggestion.pathHint ??
                    '未定位文件'}
                </span>
                <span>{suggestion.lineHint ?? '无行号'}</span>
              </div>
              <div className="dv-external-review-item-actions">
                <button
                  className="dv-inline-ann-action"
                  onClick={() => props.onJudgeOne(suggestion.id)}
                  disabled={props.busy}
                >
                  发送给 AICLI 判断
                </button>
              </div>
              {suggestion.decisionReason && (
                <div className="dv-external-review-item-reason">
                  {suggestion.decisionReason}
                </div>
              )}
            </li>
          ))}
        </ul>
      )}
    </section>
  )
}
