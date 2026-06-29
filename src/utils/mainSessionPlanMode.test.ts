import { describe, expect, it } from 'vitest'
import {
  canStartMainSession,
  formatMainSessionPlanLabel
} from './mainSessionPlanMode.js'

describe('main session plan mode', () => {
  it('allows starting with a project when no-plan mode is selected', () => {
    expect(canStartMainSession('project-1', true, '')).toBe(true)
  })

  it('still requires a plan name outside no-plan mode', () => {
    expect(canStartMainSession('project-1', false, '')).toBe(false)
    expect(canStartMainSession('project-1', false, ' fix-crash ')).toBe(true)
  })

  it('uses scheduled-task wording for no-plan session starts', () => {
    expect(formatMainSessionPlanLabel(true, '')).toBe('\u5b9a\u65f6\u4efb\u52a1')
    expect(formatMainSessionPlanLabel(false, ' fix-crash ')).toBe('fix-crash')
    expect(formatMainSessionPlanLabel(false, '')).toBe('(未选择普通任务)')
  })
})
