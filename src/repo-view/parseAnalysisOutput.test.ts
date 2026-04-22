import { describe, expect, it } from 'vitest'
import { parseAnalysisOutput } from './parseAnalysisOutput.js'

describe('parseAnalysisOutput', () => {
  it('splits visible answer from memory block and end marker', () => {
    const out = parseAnalysisOutput(
      ['## 分析\nanswer text\n', '[[MEMORY_UPDATE]]\nsummary text\n[[END_OF_ANALYSIS]]'].join('')
    )
    expect(out.answer).toContain('answer text')
    expect(out.memoryUpdate).toContain('summary text')
    expect(out.complete).toBe(true)
  })
})
