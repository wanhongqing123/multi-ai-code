import { describe, expect, it } from 'vitest'
import {
  formatDecisionErrorForDisplay,
  formatDecisionForDisplay,
  formatDecisionReasonForDisplay,
  matchSuggestionsToDiffFiles,
  parseExternalReviewSuggestions
} from '../../../src/components/externalAiReview.js'

describe('parseExternalReviewSuggestions', () => {
  it('keeps imported markdown review as one whole suggestion', () => {
    const suggestions = parseExternalReviewSuggestions(
      '# Summary\n\n- item 1 in src/App.tsx line 42\n- item 2 in electron/main.ts',
      'Claude review'
    )

    expect(suggestions).toHaveLength(1)
    expect(suggestions[0]).toEqual(
      expect.objectContaining({
        id: expect.any(String),
        sourceLabel: 'Claude review',
        pathHint: 'src/App.tsx',
        lineHint: '42',
        linkedDiffFile: null,
        status: 'idle',
        decisionReason: ''
      })
    )
  })

  it('returns empty when content is blank', () => {
    expect(parseExternalReviewSuggestions('   \n\n', 'Imported review')).toEqual([])
  })
})

describe('matchSuggestionsToDiffFiles', () => {
  it('matches a parsed path hint to a visible diff file by suffix', () => {
    const suggestions = parseExternalReviewSuggestions(
      '# AI Review\n\n- Fix the issue in src/App.tsx line 42',
      'Claude review'
    )
    const diffFiles = [{ path: 'packages/app/src/App.tsx' }, { path: 'docs/readme.md' }]
    const matched = matchSuggestionsToDiffFiles(suggestions, diffFiles)
    expect(matched[0].linkedDiffFile).toBe(diffFiles[0])
  })
})

describe('formatDecisionReasonForDisplay', () => {
  it('extracts reason from tagged JSON block and keeps output concise', () => {
    const raw = [
      'MAC_EXTERNAL_REVIEW_JSON_START',
      '{"decision":"accepted","reason":"Evidence is sufficient and actionable."}',
      'MAC_EXTERNAL_REVIEW_JSON_END'
    ].join('\n')

    expect(formatDecisionReasonForDisplay(raw)).toBe('Evidence is sufficient and actionable.')
  })
})

describe('formatDecisionErrorForDisplay', () => {
  it('hides low-level JSON parsing errors from end users', () => {
    const raw = `Unexpected token '', "[45;3HEnd"... is not valid JSON`
    expect(formatDecisionErrorForDisplay(raw)).toBe('AI 返回格式异常，请重试。')
  })

  it('does not expose raw sentinel protocol blocks', () => {
    const raw = [
      'MAC_EXTERNAL_REVIEW_JSON_START',
      '{"decision":"accepted","reason":"something"}',
      'MAC_EXTERNAL_REVIEW_JSON_END'
    ].join('\n')
    expect(formatDecisionErrorForDisplay(raw)).toBe(
      'AI 返回格式不规范，已忽略原始协议内容。请重试。'
    )
  })
})

describe('formatDecisionForDisplay', () => {
  it('renders accepted/rejected suggestions and modification plan into readable text', () => {
    const content = formatDecisionForDisplay({
      decision: 'accepted',
      reason: 'Overall review is actionable.',
      acceptedChanges: [
        {
          title: 'Guard cross-thread state',
          reason: 'There is a confirmed race condition.',
          fileHint: 'MediaPlayer.cpp',
          lineHint: '1204',
          recommendation: 'Use the existing mutex around both read and write.'
        }
      ],
      rejectedChanges: [
        {
          title: 'Refactor utility naming',
          reason: 'Low value and unrelated to current fix.'
        }
      ],
      modificationPlan: ['Fix race condition first', 'Align callback contract']
    })

    expect(content).toContain('结论：采纳')
    expect(content).toContain('需修改（建议采纳）：')
    expect(content).toContain('无需修改（不建议采纳）：')
    expect(content).toContain('建议修改方案：')
    expect(content).toContain('Guard cross-thread state')
    expect(content).toContain('Fix race condition first')
  })
})
