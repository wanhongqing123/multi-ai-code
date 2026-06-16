import { describe, expect, it } from 'vitest'
import { nextRuntimeLogDialogOpenAfterSendResult } from './runtimeLogDialogState.js'

describe('nextRuntimeLogDialogOpenAfterSendResult', () => {
  it('closes the runtime log dialog after the analysis request is sent successfully', () => {
    expect(nextRuntimeLogDialogOpenAfterSendResult(true, true)).toBe(false)
  })

  it('keeps the runtime log dialog open when the analysis request fails', () => {
    expect(nextRuntimeLogDialogOpenAfterSendResult(true, false)).toBe(true)
    expect(nextRuntimeLogDialogOpenAfterSendResult(false, false)).toBe(false)
  })
})
