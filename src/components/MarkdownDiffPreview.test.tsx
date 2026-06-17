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
    // Old / New column labels were intentionally removed to keep the
    // preview clean; the visual highlight + badge convey direction now.
    expect(html).not.toContain('dv-md-preview-head">Old<')
    expect(html).not.toContain('dv-md-preview-head">New<')
    // Headings now carry data-sourcepos="…" — assert text content via regex.
    expect(html).toMatch(/<h1[^>]*>Old<\/h1>/)
    expect(html).toMatch(/<h1[^>]*>New<\/h1>/)
    expect(html).toMatch(/<li[^>]*>one<\/li>/)
    expect(html).toMatch(/<li[^>]*>two<\/li>/)
  })

  it('renders empty-state messaging when one side has no content', () => {
    const html = renderToStaticMarkup(
      <MarkdownDiffPreview filePath="docs/guide.md" oldText="" newText={'# Added'} />
    )

    expect(html).toContain('No previous content')
    expect(html).toMatch(/<h1[^>]*>Added<\/h1>/)
  })

  it('emits data-sourcepos so rendered blocks are addressable per source line', () => {
    const html = renderToStaticMarkup(
      <MarkdownDiffPreview
        filePath="docs/guide.md"
        oldText={'# Title\n\n- one'}
        newText={'# Title\n\n- two'}
        newChangedLines={[3]}
      />
    )
    // Our rehype plugin attaches data-sourcepos to every rendered block.
    expect(html).toMatch(/data-sourcepos="1:1-1:\d+"/)
    expect(html).toMatch(/data-sourcepos="3:1-3:\d+"/)
  })

  it('injects a scoped <style> rule for each changed line in the new column', () => {
    const html = renderToStaticMarkup(
      <MarkdownDiffPreview
        filePath="docs/guide.md"
        oldText={'# Title\n\n- one'}
        newText={'# Title\n\n- two'}
        newChangedLines={[3]}
      />
    )
    // The injected <style> targets the new-side root and the changed line.
    // renderToStaticMarkup escapes `"` inside <style> content, so match the
    // entity form as well.
    expect(html).toMatch(/#md-new-[^ ]+\s*\[data-sourcepos\^=(?:"|&quot;)3:/)
    // Old side has no changed lines here, so no style block for old.
    expect(html).not.toMatch(/#md-old-[^ ]+\s*\[data-sourcepos\^=(?:"|&quot;)/)
  })

  it('injects scoped style rules for old side when there are removed lines', () => {
    const html = renderToStaticMarkup(
      <MarkdownDiffPreview
        filePath="docs/guide.md"
        oldText={'# Title\n\n- gone'}
        newText={'# Title\n'}
        oldChangedLines={[3]}
      />
    )
    expect(html).toMatch(/#md-old-[^ ]+\s*\[data-sourcepos\^=(?:"|&quot;)3:/)
  })

  it('shows a +N / -N badge summarizing changed lines', () => {
    const html = renderToStaticMarkup(
      <MarkdownDiffPreview
        filePath="docs/guide.md"
        oldText={'a\nb\nc'}
        newText={'a\nB\nC'}
        oldChangedLines={[2, 3]}
        newChangedLines={[2, 3]}
      />
    )
    expect(html).toContain('dv-md-preview-badge')
    // Order is removed first, then added.
    expect(html).toMatch(/-2\s*\+2/)
  })

  it('does not show the badge when there are no changed lines', () => {
    const html = renderToStaticMarkup(
      <MarkdownDiffPreview
        filePath="docs/guide.md"
        oldText={'# Same'}
        newText={'# Same'}
      />
    )
    expect(html).not.toContain('dv-md-preview-badge')
  })

  it('uses separate scoped ids so old and new highlights cannot collide', () => {
    const html = renderToStaticMarkup(
      <MarkdownDiffPreview
        filePath="docs/guide.md"
        oldText={'# Old\n\n- one'}
        newText={'# New\n\n- two'}
        oldChangedLines={[1]}
        newChangedLines={[1]}
      />
    )
    const oldMatch = html.match(/id="(md-old-[^"]+)"/)
    const newMatch = html.match(/id="(md-new-[^"]+)"/)
    expect(oldMatch).not.toBeNull()
    expect(newMatch).not.toBeNull()
    expect(oldMatch![1]).not.toBe(newMatch![1])
  })

  it('renders mermaid sequenceDiagram fences as diagrams in preview mode', () => {
    const diagram = `\`\`\`mermaid
sequenceDiagram
    participant C as GLVideoConsumer
    participant F as PlatformObjectFactory
    C->>F: createVideoRenderer(buffer)
    F-->>C: new GLSwVideoRenderer(Windows)
\`\`\``
    const html = renderToStaticMarkup(
      <MarkdownDiffPreview filePath="docs/guide.md" oldText="" newText={diagram} />
    )

    expect(html).toContain('markdown-mermaid-diagram')
    expect(html).toContain('markdown-mermaid-loading')
    expect(html).not.toContain('<pre')
    expect(html).not.toContain('GLVideoConsumer')
    expect(html).not.toContain('createVideoRenderer(buffer)')
    expect(html).not.toContain('sequenceDiagram')
  })
})
