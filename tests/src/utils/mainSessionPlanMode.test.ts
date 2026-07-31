import { describe, expect, it } from 'vitest'
import { canStartMainSession } from '../../../src/utils/mainSessionPlanMode.js'

describe('canStartMainSession', () => {
  it('only needs a project', () => {
    // 普通任务不是启动前提：没选任务也能起会话。顶栏此前会写「(未选择普通
    // 任务)」，那句话既不影响能不能启动，也不提供任何信息，已经去掉了。
    expect(canStartMainSession('project-1')).toBe(true)
    expect(canStartMainSession(null)).toBe(false)
  })
})
