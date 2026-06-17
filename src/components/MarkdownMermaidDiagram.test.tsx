import { describe, expect, it } from 'vitest'
import { formatMermaidErrorMessage } from './MarkdownMermaidDiagram.js'

describe('formatMermaidErrorMessage', () => {
  it('includes parser location details when Mermaid exposes them', () => {
    const error = Object.assign(new Error('Syntax error in text'), {
      hash: {
        loc: {
          first_line: 7,
          first_column: 5
        },
        token: 'ALPHA'
      }
    })

    expect(formatMermaidErrorMessage(error)).toBe(
      'Syntax error in text (line 7, column 5, token ALPHA)'
    )
  })

  it('falls back to the error message when parser details are absent', () => {
    expect(formatMermaidErrorMessage(new Error('Render failed'))).toBe('Render failed')
    expect(formatMermaidErrorMessage('Unknown failure')).toBe('Unknown failure')
  })
})
