import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it } from 'vitest'
import MarkdownDiffPreview from './MarkdownDiffPreview.js'

describe('MarkdownDiffPreview', () => {
  it('renders old and new markdown columns', () => {
    const html = renderToStaticMarkup(
      <MarkdownDiffPreview
        filePath="docs/guide.md"
        oldText={'# Old\n\n- one'}
        newText={'# New\n\n- two'}
      />
    )

    expect(html).toContain('Markdown Preview')
    expect(html).toContain('dv-md-preview-col')
    expect(html).toContain('dv-md-preview-head">Old<')
    expect(html).toContain('dv-md-preview-head">New<')
    expect(html).toContain('<h1>Old</h1>')
    expect(html).toContain('<h1>New</h1>')
    expect(html).toContain('<li>one</li>')
    expect(html).toContain('<li>two</li>')
  })

  it('renders empty-state messaging when one side has no content', () => {
    const html = renderToStaticMarkup(
      <MarkdownDiffPreview filePath="docs/guide.md" oldText="" newText={'# Added'} />
    )

    expect(html).toContain('No previous content')
    expect(html).toContain('<h1>Added</h1>')
  })
})
