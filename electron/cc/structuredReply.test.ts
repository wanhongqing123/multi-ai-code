import { describe, expect, it } from 'vitest'
import {
  JSON_REPLY_END,
  JSON_REPLY_START,
  buildExternalReviewPrompt,
  extractTaggedJsonReply,
  type ExternalReviewPromptArgs
} from './structuredReply.js'

describe('buildExternalReviewPrompt', () => {
  it('builds a prompt that requests a single tagged JSON decision block without embedding a tagged sample reply', () => {
    const args: ExternalReviewPromptArgs = {
      planAbsPath: 'C:/plans/review-plan.md',
      suggestion: {
        rawText: 'Consider accepting the patch after verifying the null guard.',
        pathHint: 'src/app.ts',
        lineHint: '42-45',
        linkedDiffFile: { path: 'C:/tmp/review.diff' }
      }
    }
    const prompt = buildExternalReviewPrompt(args)

    expect(prompt).toContain(JSON_REPLY_START)
    expect(prompt).toContain(JSON_REPLY_END)
    expect(prompt).toContain('"decision":"accepted|rejected|needs-human"')
    expect(prompt).toContain('"reason":"..."')
    expect(prompt).toContain('single JSON object')
    expect(prompt).toContain('C:/plans/review-plan.md')
    expect(prompt).toContain('src/app.ts')
    expect(prompt).toContain('42-45')
    expect(prompt).toContain('C:/tmp/review.diff')
    expect(prompt).not.toContain(
      `${JSON_REPLY_START}\n{"decision":"accepted|rejected|needs-human","reason":"..."}\n${JSON_REPLY_END}`
    )
  })

  it('handles the renderer suggestion shape with nullable fields', () => {
    const args: ExternalReviewPromptArgs = {
      planAbsPath: 'C:/plans/review-plan.md',
      suggestion: {
        rawText: 'This suggestion has no matched path yet.',
        pathHint: null,
        lineHint: null,
        linkedDiffFile: null
      }
    }

    const prompt = buildExternalReviewPrompt(args)

    expect(prompt).toContain('Path hint: (none)')
    expect(prompt).toContain('Line hint: (none)')
    expect(prompt).toContain('Linked diff file: (none)')
  })
})

describe('extractTaggedJsonReply', () => {
  it('returns null until the tagged JSON reply is complete', () => {
    expect(extractTaggedJsonReply(`noise\n${JSON_REPLY_START}\n{"decision":"accepted"`)).toBeNull()
    expect(
      extractTaggedJsonReply(
        `noise\n${JSON_REPLY_START}\n{"decision":"accepted","reason":"looks good"}`
      )
    ).toBeNull()
  })

  it('extracts and validates a tagged JSON reply from terminal output', () => {
    const output = [
      'streamed terminal output',
      JSON_REPLY_START,
      '{"decision":"needs-human","reason":"The suggestion references a file that is not in the diff."}',
      JSON_REPLY_END,
      'more streamed output'
    ].join('\n')

    expect(extractTaggedJsonReply(output)).toEqual({
      decision: 'needs-human',
      reason: 'The suggestion references a file that is not in the diff.'
    })
  })

  it('rejects invalid decisions and empty reasons', () => {
    const invalidDecision = [
      JSON_REPLY_START,
      '{"decision":"maybe","reason":"unclear"}',
      JSON_REPLY_END
    ].join('\n')
    expect(() => extractTaggedJsonReply(invalidDecision)).toThrow(/decision/i)

    const emptyReason = [
      JSON_REPLY_START,
      '{"decision":"accepted","reason":"   "}',
      JSON_REPLY_END
    ].join('\n')
    expect(() => extractTaggedJsonReply(emptyReason)).toThrow(/reason/i)
  })
})
