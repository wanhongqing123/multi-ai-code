import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import ExternalAiReviewPanel from './ExternalAiReviewPanel.js'

describe('ExternalAiReviewPanel', () => {
  it('renders imported suggestions with status and reason', () => {
    const markup = renderToStaticMarkup(
      <ExternalAiReviewPanel
        sourceLabel="review.md"
        suggestions={[
          {
            id: 's1',
            sourceLabel: 'review.md',
            rawText: 'src/App.tsx line 42: rename this state',
            pathHint: 'src/App.tsx',
            lineHint: '42',
            linkedDiffFile: { path: 'src/App.tsx' },
            status: 'accepted',
            decisionReason: 'The rename reduces ambiguity.'
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
    expect(markup).toContain('The rename reduces ambiguity.')
  })
})
