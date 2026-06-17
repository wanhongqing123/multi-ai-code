import { describe, expect, it } from 'vitest'
import {
  firstMeaningfulMermaidLine,
  getRenderableMermaidChart,
  isMermaidCodeBlock,
  isMermaidSequenceDiagram,
  normalizeMixedMermaidMarkdown,
  splitSequenceDiagramMixedContent
} from './markdownMermaid.js'

describe('markdownMermaid', () => {
  it('finds the first non-empty non-comment mermaid line', () => {
    expect(firstMeaningfulMermaidLine('\n%% comment\nsequenceDiagram\nA->>B: hi')).toBe(
      'sequenceDiagram'
    )
  })

  it('detects mermaid sequence diagram fences', () => {
    const sequence = 'sequenceDiagram\nparticipant A\nA->>A: render'

    expect(isMermaidCodeBlock(sequence, 'language-mermaid')).toBe(true)
    expect(isMermaidCodeBlock('graph TD\nA-->B', 'language-mermaid')).toBe(true)
    expect(isMermaidSequenceDiagram(sequence, 'language-mermaid')).toBe(true)
    expect(isMermaidSequenceDiagram(sequence, undefined)).toBe(true)
    expect(isMermaidSequenceDiagram('graph TD\nA-->B', 'language-mermaid')).toBe(false)
  })

  it('splits prose that follows a sequence diagram', () => {
    const mixed = `sequenceDiagram
    participant A
    A->>A: render

关键: this should stay markdown`

    expect(splitSequenceDiagramMixedContent(mixed)).toEqual({
      diagram: 'sequenceDiagram\n    participant A\n    A->>A: render',
      trailing: '\n关键: this should stay markdown',
      diagramLineCount: 3
    })
  })

  it('extracts a renderable chart from mixed mermaid text', () => {
    const mixed = `sequenceDiagram
    participant A
    A->>A: render
关键: this should stay markdown`

    expect(getRenderableMermaidChart(mixed)).toBe(
      'sequenceDiagram\n    participant A\n    A->>A: render'
    )
  })

  it('keeps sequence metadata lines inside the renderable chart', () => {
    const mixed = `sequenceDiagram
    title Renderer setup
    accTitle: Renderer setup sequence
    accDescr: Shows renderer init order
    participant A
    note over A: lower-case note stays in the diagram
    A->>A: render
plain prose: this should stay markdown`

    expect(getRenderableMermaidChart(mixed)).toBe(`sequenceDiagram
    title Renderer setup
    accTitle: Renderer setup sequence
    accDescr: Shows renderer init order
    participant A
    note over A: lower-case note stays in the diagram
    A->>A: render`)
  })

  it('escapes semicolons in sequence text labels before rendering', () => {
    const mixed = `sequenceDiagram
    participant R
    Note over R: branch one; branch two
    R->>R: upload Y; upload U
plain prose: this should stay markdown`

    expect(getRenderableMermaidChart(mixed)).toBe(`sequenceDiagram
    participant R
    Note over R: branch one#59; branch two
    R->>R: upload Y#59; upload U`)
  })

  it('normalizes bare sequence diagrams inside mixed markdown', () => {
    const markdown = `# Review

sequenceDiagram
    participant A
    A->>A: render

关键: this should stay markdown`

    expect(normalizeMixedMermaidMarkdown(markdown)).toBe(`# Review

\`\`\`mermaid
sequenceDiagram
    participant A
    A->>A: render
\`\`\`

关键: this should stay markdown`)
  })

  it('moves trailing prose out of a mermaid fence when it follows a sequence diagram', () => {
    const markdown = `\`\`\`mermaid
sequenceDiagram
    participant A
    A->>A: render
关键: this should stay markdown
\`\`\``

    expect(normalizeMixedMermaidMarkdown(markdown)).toBe(`\`\`\`mermaid
sequenceDiagram
    participant A
    A->>A: render
\`\`\`
关键: this should stay markdown`)
  })
})
