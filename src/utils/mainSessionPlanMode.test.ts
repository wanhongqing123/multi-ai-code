import { describe, expect, it } from 'vitest'
import {
  canStartMainSession,
  formatMainSessionPlanLabel,
  NO_PLAN_SELECT_VALUE
} from './mainSessionPlanMode.js'

describe('main session plan mode', () => {
  it('allows starting with a project when no-plan mode is selected', () => {
    expect(canStartMainSession('project-1', true, '')).toBe(true)
  })

  it('still requires a plan name outside no-plan mode', () => {
    expect(canStartMainSession('project-1', false, '')).toBe(false)
    expect(canStartMainSession('project-1', false, ' fix-crash ')).toBe(true)
  })

  it('keeps no-plan selection and display explicit', () => {
    expect(NO_PLAN_SELECT_VALUE).toBe('__NO_PLAN__')
    expect(formatMainSessionPlanLabel(true, '')).toBe('无方案模式')
    expect(formatMainSessionPlanLabel(false, ' fix-crash ')).toBe('fix-crash')
    expect(formatMainSessionPlanLabel(false, '')).toBe('(未选择方案)')
  })
})
