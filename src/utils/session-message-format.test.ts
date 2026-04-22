import { describe, expect, it } from 'vitest'
import { formatInitialMessage } from './session-message-format.js'

describe('formatInitialMessage', () => {
  it('uses path-only instructions for external plans', () => {
    const out = formatInitialMessage({
      planName: 'vendor-plan',
      planAbsPath: '/external/vendor-plan.md',
      planSource: 'external',
      planContent: '# 外部方案\n\n这里不应直接发给 AI'
    })

    expect(out).toContain('/external/vendor-plan.md')
    expect(out).toContain('请先自行读取这个方案文件')
    expect(out).not.toContain('这里不应直接发给 AI')
  })
})
