import { describe, expect, it } from 'vitest'
import { matchSuggestionsToDiffFiles, parseExternalReviewSuggestions } from './externalAiReview.js'

describe('parseExternalReviewSuggestions', () => {
  it('splits markdown bullets into separate suggestions', () => {
    const suggestions = parseExternalReviewSuggestions(
      '- Fix the button spacing in src/App.tsx line 42\n- Update the copy in electron/main.ts',
      'Claude review'
    )

    expect(suggestions).toHaveLength(2)
    expect(suggestions[0]).toEqual(
      expect.objectContaining({
        id: expect.any(String),
        sourceLabel: 'Claude review',
        rawText: 'Fix the button spacing in src/App.tsx line 42',
        pathHint: 'src/App.tsx',
        lineHint: '42',
        linkedDiffFile: null,
        status: 'idle',
        decisionReason: ''
      })
    )
    expect(suggestions[1]).toEqual(
      expect.objectContaining({
        rawText: 'Update the copy in electron/main.ts',
        pathHint: 'electron/main.ts',
        lineHint: null,
        linkedDiffFile: null,
        status: 'idle',
        decisionReason: ''
      })
    )
  })

  it('ignores intro text when bullet structure is present', () => {
    const suggestions = parseExternalReviewSuggestions(
      'Summary\n\n- item 1\n- item 2',
      'Claude review'
    )

    expect(suggestions).toHaveLength(2)
    expect(suggestions.map((suggestion) => suggestion.rawText)).toEqual(['item 1', 'item 2'])
  })

  it('splits blank-line paragraphs when the review is not bullet structured', () => {
    const suggestions = parseExternalReviewSuggestions(
      'First paragraph about src/App.tsx line 120-128.\n\nSecond paragraph.\n\n# Heading only\n\nThird paragraph.',
      'Imported review'
    )

    expect(suggestions).toHaveLength(3)
    expect(suggestions.map((suggestion) => suggestion.rawText)).toEqual([
      'First paragraph about src/App.tsx line 120-128.',
      'Second paragraph.',
      'Third paragraph.'
    ])
    expect(suggestions[0]).toEqual(
      expect.objectContaining({
        pathHint: 'src/App.tsx',
        lineHint: '120-128'
      })
    )
  })

  it('assigns deterministic ids for the same input', () => {
    const first = parseExternalReviewSuggestions('- item 1', 'Imported review')
    const second = parseExternalReviewSuggestions('- item 1', 'Imported review')

    expect(first[0].id).toBe(second[0].id)
  })
})

describe('matchSuggestionsToDiffFiles', () => {
  it('matches a parsed path hint to a visible diff file by suffix', () => {
    const suggestions = parseExternalReviewSuggestions(
      '- Fix the issue in src/App.tsx line 42',
      'Claude review'
    )
    const diffFiles = [{ path: 'packages/app/src/App.tsx' }, { path: 'docs/readme.md' }]

    const matched = matchSuggestionsToDiffFiles(suggestions, diffFiles)

    expect(matched[0].linkedDiffFile).toBe(diffFiles[0])
  })

  it('does not link ambiguous suffix matches arbitrarily', () => {
    const suggestions = parseExternalReviewSuggestions('- Fix src/App.tsx', 'Claude review')
    const diffFiles = [{ path: 'packages/a/src/App.tsx' }, { path: 'packages/b/src/App.tsx' }]

    const matched = matchSuggestionsToDiffFiles(suggestions, diffFiles)

    expect(matched[0].linkedDiffFile).toBe(null)
  })

  it('prefers exact match over suffix candidates', () => {
    const suggestions = parseExternalReviewSuggestions('- Fix src/App.tsx', 'Claude review')
    const diffFiles = [
      { path: 'packages/a/src/App.tsx' },
      { path: 'src/App.tsx' },
      { path: 'packages/b/src/App.tsx' }
    ]

    const matched = matchSuggestionsToDiffFiles(suggestions, diffFiles)

    expect(matched[0].linkedDiffFile).toBe(diffFiles[1])
  })
})
