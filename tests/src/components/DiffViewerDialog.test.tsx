import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import DiffViewerDialog from '../../../src/components/DiffViewerDialog.js'

describe('DiffViewerDialog', () => {
  it('does not render the redundant close-without-send footer button', () => {
    const markup = renderToStaticMarkup(
      <DiffViewerDialog
        cwd="E:/OpenSource/multi-ai-code"
        onClose={vi.fn()}
        onSubmit={vi.fn()}
        sessionRunning={true}
        annotations={[]}
        onAnnotationsChange={vi.fn()}
        generalNote=""
        onGeneralNoteChange={vi.fn()}
        mode="working"
        onModeChange={vi.fn()}
        selectedCommit=""
        onSelectedCommitChange={vi.fn()}
        selectedFile=""
        onSelectedFileChange={vi.fn()}
        onJudgeExternalReviewItem={vi.fn(
          async () =>
            ({
              ok: true,
              result: { decision: 'accepted', reason: 'Looks good.' }
            }) as const
        )}
      />
    )

    expect(markup).not.toContain('鍏抽棴锛堜笉鍙戦€侊級')
  })

  it('renders markdown preview content for markdown files when preview mode is active', () => {
    const markup = renderToStaticMarkup(
      <DiffViewerDialog
        cwd="E:/OpenSource/multi-ai-code"
        onClose={vi.fn()}
        onSubmit={vi.fn()}
        sessionRunning={true}
        annotations={[]}
        onAnnotationsChange={vi.fn()}
        generalNote=""
        onGeneralNoteChange={vi.fn()}
        mode="working"
        onModeChange={vi.fn()}
        selectedCommit=""
        onSelectedCommitChange={vi.fn()}
        selectedFile="docs/guide.md"
        onSelectedFileChange={vi.fn()}
        onJudgeExternalReviewItem={vi.fn(
          async () =>
            ({
              ok: true,
              result: { decision: 'accepted', reason: 'Looks good.' }
            }) as const
        )}
        initialDiffText={[
          'diff --git a/docs/guide.md b/docs/guide.md',
          '--- a/docs/guide.md',
          '+++ b/docs/guide.md',
          '@@ -1,3 +1,3 @@',
          ' # Guide',
          '-- old item',
          '+- new item',
          ' '
        ].join('\n')}
        initialFileViewMode="markdown-preview"
      />
    )

    expect(markup).toContain('Markdown Preview')
    expect(markup).toContain('dv-file-view-tab active')
    expect(markup).toContain('dv-md-preview')
    // <li> now carries data-sourcepos for the diff-highlight overlay.
    expect(markup).toMatch(/<li[^>]*>old item<\/li>/)
    expect(markup).toMatch(/<li[^>]*>new item<\/li>/)
    expect(markup).not.toContain('dv-file-rows')
  })

  it('renders external review panel section in diff dialog', () => {
    const markup = renderToStaticMarkup(
      <DiffViewerDialog
        cwd="E:/OpenSource/multi-ai-code"
        onClose={vi.fn()}
        onSubmit={vi.fn()}
        sessionRunning={true}
        annotations={[]}
        onAnnotationsChange={vi.fn()}
        generalNote=""
        onGeneralNoteChange={vi.fn()}
        mode="working"
        onModeChange={vi.fn()}
        selectedCommit=""
        onSelectedCommitChange={vi.fn()}
        selectedFile=""
        onSelectedFileChange={vi.fn()}
        onJudgeExternalReviewItem={vi.fn(
          async () =>
            ({
              ok: true,
              result: { decision: 'accepted', reason: 'Looks good.' }
            }) as const
        )}
      />
    )

    expect(markup).toContain('dv-external-review')
  })
})
