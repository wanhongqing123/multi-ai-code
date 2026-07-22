import { describe, expect, it } from 'vitest'
import {
  nextRuntimeLogCommentAfterSendResult,
  nextRuntimeLogDialogOpenAfterSendResult
} from '../../../src/utils/runtimeLogDialogState.js'

describe('nextRuntimeLogDialogOpenAfterSendResult', () => {
  it('closes the runtime log dialog after the analysis request is sent successfully', () => {
    expect(nextRuntimeLogDialogOpenAfterSendResult(true, true)).toBe(false)
  })

  it('keeps the runtime log dialog open when the analysis request fails', () => {
    expect(nextRuntimeLogDialogOpenAfterSendResult(true, false)).toBe(true)
    expect(nextRuntimeLogDialogOpenAfterSendResult(false, false)).toBe(false)
  })

  it('clears the previous supplemental comment after the analysis request is sent successfully', () => {
    expect(nextRuntimeLogCommentAfterSendResult('why did video not play?', true)).toBe('')
  })

  it('keeps the supplemental comment when the analysis request fails', () => {
    expect(nextRuntimeLogCommentAfterSendResult('retry this question', false)).toBe('retry this question')
  })
})
