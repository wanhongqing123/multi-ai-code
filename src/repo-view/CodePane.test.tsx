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
  it('renders header metadata and code-search controls', () => {
    const markup = renderPane()

    expect(markup).toContain('<div class="repo-code-head"')
    expect(markup).toContain('<span class="repo-code-path">src/repo-view/CodePane.tsx</span>')
    expect(markup).toContain('<span class="repo-code-meta">2 行</span>')
    expect(markup).toContain('<span class="repo-code-meta">24 bytes</span>')
    expect(markup).toContain('class="repo-code-search-input"')
    expect(markup).toContain('placeholder="搜索代码"')
    expect(markup).toContain('<span class="repo-code-search-count">0</span>')
  })

  it('highlights keywords and function names in code lines', () => {
    const markup = renderPane({
      content: 'function runTask() {\n  return processItems();\n}'
    })

    expect(markup).toContain('repo-code-token-keyword')
    expect(markup).toContain('repo-code-token-function')
    expect(markup).toContain('runTask')
    expect(markup).toContain('processItems')
  })

  it('renders markdown content for .md files', () => {
    const markup = renderPane({
      filePath: 'docs/guide.md',
      content: '# Title\n\n- item 1\n- item 2\n\n[OpenAI](https://openai.com)'
    })

    expect(markup).toContain('class="repo-code-markdown md-rendered"')
    expect(markup).toContain('<h1>Title</h1>')
    expect(markup).toContain('<li>item 1</li>')
    expect(markup).toContain('href="https://openai.com"')
    expect(markup).not.toContain('class="repo-code-pre"')
    expect(markup).not.toContain('placeholder="搜索代码"')
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
