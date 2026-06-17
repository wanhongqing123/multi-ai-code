import { describe, expect, it } from 'vitest'
import {
  firstMeaningfulMermaidLine,
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
