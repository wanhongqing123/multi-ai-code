import { describe, expect, it } from 'vitest'
import {
  firstMeaningfulMermaidLine,
  isMermaidCodeBlock,
  isMermaidSequenceDiagram
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
})
