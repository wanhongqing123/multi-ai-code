import { describe, expect, it } from 'vitest'
import { ALL_HABIT_EVENT_KINDS, type HabitEventKind, type SkillCandidateStatus } from './db.js'

describe('habit event kinds', () => {
  it('declares exactly 7 distinct event kinds', () => {
    expect(ALL_HABIT_EVENT_KINDS).toHaveLength(7)
    expect(new Set(ALL_HABIT_EVENT_KINDS).size).toBe(7)
  })

  it('keeps the agreed set of kinds in sync with the design doc', () => {
    expect(ALL_HABIT_EVENT_KINDS).toEqual<HabitEventKind[]>([
      'pty_cmd',
      'ai_prompt_main',
      'ai_prompt_repo',
      'diff_annotation',
      'repo_view_annotation',
      'template_used',
      'plan_imported'
    ])
  })
})

describe('skill candidate status values', () => {
  it('uses only the allowed status literals', () => {
    const allowed: SkillCandidateStatus[] = [
      'pending',
      'accepted',
      'edited',
      'discarded',
      'snoozed',
      'error'
    ]
    expect(new Set(allowed).size).toBe(6)
  })
})
