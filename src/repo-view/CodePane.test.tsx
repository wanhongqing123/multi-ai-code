import type { ComponentProps } from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import CodePane from './CodePane.js'

function renderPane(
  overrides: Partial<ComponentProps<typeof CodePane>> = {}
): string {
  return renderToStaticMarkup(
    <CodePane
      filePath="src/repo-view/CodePane.tsx"
      content={'first line\nsecond line'}
      byteLength={24}
      loading={false}
      onAnnotateSelection={vi.fn()}
      onCancelEditing={vi.fn()}
      {...overrides}
    />
  )
}

describe('CodePane', () => {
  it('renders the file context header with grouped path and meta content', () => {
    const markup = renderPane()

    expect(markup).toContain('<div class="repo-code-head"')
    expect(markup).toContain('<span class="repo-code-head-main">')
    expect(markup).toContain('<span class="repo-code-path">src/repo-view/CodePane.tsx</span>')
    expect(markup).toContain(`<span class="repo-code-meta">2 ${'\u884c'}</span>`)
    expect(markup).toContain('<span class="repo-code-meta">24 bytes</span>')
  })

  it('marks only the lines inside the editing annotation range as linked', () => {
    const markup = renderPane({
      content: 'one\ntwo\nthree\nfour',
      editingAnnotation: {
        id: 'ann_1',
        lineRange: '2-3',
        snippet: 'two\nthree',
        comment: 'Review this block.'
      }
    })

    expect(markup).toContain('<div class="repo-code-line" data-line="1">')
    expect(markup).toContain('<div class="repo-code-line linked" data-line="2">')
    expect(markup).toContain('<div class="repo-code-line linked" data-line="3">')
    expect(markup).toContain('<div class="repo-code-line" data-line="4">')
    expect(markup.match(/repo-code-line linked/g)?.length).toBe(2)
  })
})
