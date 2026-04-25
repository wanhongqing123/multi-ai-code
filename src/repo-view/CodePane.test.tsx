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

    expect(markup).toContain('repo-code-head-main')
    expect(markup).toContain('repo-code-path')
    expect(markup).toContain(`2 ${'\u884c'}`)
  })

  it('marks lines in the editing annotation range as linked', () => {
    const markup = renderPane({
      content: 'one\ntwo\nthree',
      editingAnnotation: {
        id: 'ann_1',
        lineRange: '2-3',
        snippet: 'two\nthree',
        comment: 'Review this block.'
      }
    })

    expect(markup).toContain('repo-code-line linked')
  })
})
