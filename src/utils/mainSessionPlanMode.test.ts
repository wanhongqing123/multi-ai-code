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

  it('uses task-watch wording for no-plan session starts', () => {
    expect(formatMainSessionPlanLabel(true, '')).toBe('\u4efb\u52a1\u503c\u5b88\u6a21\u5f0f')
    expect(formatMainSessionPlanLabel(false, ' fix-crash ')).toBe('fix-crash')
    expect(formatMainSessionPlanLabel(false, '')).toBe('(\u672a\u9009\u62e9\u65b9\u6848)')
  })
})
