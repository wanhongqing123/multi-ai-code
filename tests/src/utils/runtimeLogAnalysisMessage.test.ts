import { describe, expect, it } from 'vitest'
import { buildRuntimeLogAnalysisMessage } from '../../../src/utils/runtimeLogAnalysisMessage.js'

describe('buildRuntimeLogAnalysisMessage', () => {
  it('appends the optional user comment to the runtime log file message', () => {
    expect(
      buildRuntimeLogAnalysisMessage(
        'Please read and analyze this runtime log prompt file:\nC:\\Temp\\runtime-log.md',
        '帮我分析下这个日志，然后解释下为什么视频没有播放出来。'
      )
    ).toContain('User question/comment:\n帮我分析下这个日志，然后解释下为什么视频没有播放出来。')
  })

  it('returns the base message when the comment is blank', () => {
    expect(buildRuntimeLogAnalysisMessage('base message', '   ')).toBe('base message')
  })
})
