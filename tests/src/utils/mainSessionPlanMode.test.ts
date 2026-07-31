import { describe, expect, it } from 'vitest'
import {
  canStartMainSession,
  formatMainSessionPlanLabel
} from '../../../src/utils/mainSessionPlanMode.js'

describe('main session plan label', () => {
  it('only needs a project to start a session', () => {
    // 普通任务不是启动前提：没选任务也能起会话，选了照常起。
    expect(canStartMainSession('project-1')).toBe(true)
    expect(canStartMainSession(null)).toBe(false)
  })

  it('shows the selected normal task, or says none is selected', () => {
    // 这里只描述"选没选普通任务"。定时任务与普通任务并存、在后台照常调度，
    // 标题栏显示什么与它无关——不再有"定时任务模式"这种说法。
    expect(formatMainSessionPlanLabel(' fix-crash ')).toBe('fix-crash')
    expect(formatMainSessionPlanLabel('')).toBe('(未选择普通任务)')
    expect(formatMainSessionPlanLabel('   ')).toBe('(未选择普通任务)')
  })
})
