import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import ExternalAiReviewPanel from './ExternalAiReviewPanel.js'

describe('ExternalAiReviewPanel', () => {
  it('renders markdown plus structured decision tables', () => {
    const markup = renderToStaticMarkup(
      <ExternalAiReviewPanel
        sourceLabel="review.md"
        sourcePath="/home/Administrator/Apollo/u3player/review/202605/hongqingwan_20260506.md"
        suggestions={[
          {
            id: 's1',
            sourceLabel: 'review.md',
            rawText: '# Review\n\n- src/App.tsx line 42: rename this state',
            pathHint: 'C:\\work\\repo\\src\\App.tsx',
            lineHint: '42',
            linkedDiffFile: { path: 'C:\\work\\repo\\src\\App.tsx' },
            status: 'accepted',
            decisionReason: 'The suggestion is valid.',
            decisionPayload: {
              decision: 'accepted',
              reason: 'The suggestion is valid.',
              acceptedChanges: [
                {
                  title: 'Rename state variable',
                  reason: 'Current name is ambiguous.',
                  fileHint: 'src/App.tsx',
                  lineHint: '42',
                  recommendation: 'Use a clearer state name.'
                }
              ],
              rejectedChanges: [
                {
                  title: 'Refactor unrelated helper',
                  reason: 'Not part of this patch scope.'
                }
              ],
              modificationPlan: ['Apply rename', 'Update dependent references']
            }
          }
        ]}
        busy={false}
        onImport={vi.fn()}
        onJudgeOne={vi.fn()}
        onJudgeAll={vi.fn()}
      />
    )

    expect(markup).toContain('dv-external-review')
    expect(markup).toContain('review.md')
    expect(markup).toContain('status-accepted')
    expect(markup).toContain('<h1>Review</h1>')
    expect(markup).toContain('dv-external-review-result-table')
    expect(markup).toContain('Rename state variable')
    expect(markup).toContain('Apply rename')
  })

  it('disables judge/view actions when no content or no idle suggestions', () => {
    const emptyMarkup = renderToStaticMarkup(
      <ExternalAiReviewPanel
        sourceLabel="review.md"
        suggestions={[]}
        busy={false}
        onImport={vi.fn()}
        onJudgeOne={vi.fn()}
        onJudgeAll={vi.fn()}
      />
    )
    expect(emptyMarkup).toContain('disabled')

    const judgedMarkup = renderToStaticMarkup(
      <ExternalAiReviewPanel
        sourceLabel="review.md"
        suggestions={[
          {
            id: 's1',
            sourceLabel: 'review.md',
            rawText: 'already judged',
            pathHint: null,
            lineHint: null,
            linkedDiffFile: null,
            status: 'accepted',
            decisionReason: 'done',
            decisionPayload: null
          }
        ]}
        busy={false}
        onImport={vi.fn()}
        onJudgeOne={vi.fn()}
        onJudgeAll={vi.fn()}
      />
    )
    expect(judgedMarkup).toContain('disabled')
  })
})
