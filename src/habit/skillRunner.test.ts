import { beforeEach, describe, expect, it, vi } from 'vitest'
import { requiredVariables, runSkill, type SkillRunDeps } from './skillRunner'
import type { Skill } from './skillTypes'

function makeSkill(overrides: Partial<Skill> = {}): Skill {
  return {
    id: 1,
    name: 'test',
    description: null,
    trigger: null,
    steps: [],
    source: 'manual',
    candidateId: null,
    createdAt: 0,
    updatedAt: 0,
    lastUsedAt: null,
    ...overrides
  }
}

interface FakeBus {
  fireChunk: (sessionId: string, chunk: string) => void
  listenerCount: () => number
}

function makeDeps(
  overrides: Partial<SkillRunDeps> = {}
): { deps: SkillRunDeps; sendUser: ReturnType<typeof vi.fn>; bus: FakeBus; touchLastUsed: ReturnType<typeof vi.fn> } {
  const listeners = new Set<(evt: { sessionId: string; chunk: string }) => void>()
  const bus: FakeBus = {
    fireChunk(sessionId, chunk) {
      for (const cb of listeners) cb({ sessionId, chunk })
    },
    listenerCount: () => listeners.size
  }
  const sendUser = vi.fn(async () => ({ ok: true as const }))
  const touchLastUsed = vi.fn(async () => {})
  const deps: SkillRunDeps = {
    sendUser,
    onData: (cb) => {
      listeners.add(cb)
      return () => {
        listeners.delete(cb)
      }
    },
    touchLastUsed,
    ...overrides
  }
  return { deps, sendUser, bus, touchLastUsed }
}

describe('runSkill', () => {
  beforeEach(() => {
    vi.useFakeTimers()
  })

  it('sends every prompt step in order, substituting variables', async () => {
    const { deps, sendUser } = makeDeps()
    const skill = makeSkill({
      steps: [
        { type: 'prompt', text: '看一下 {file}' },
        { type: 'prompt', text: '修复其中关于 {topic} 的部分' }
      ]
    })

    const p = runSkill(deps, {
      sessionId: 's1',
      skill,
      vars: { file: 'app.ts', topic: '性能' },
      defaultWaitMs: 100
    })
    await vi.runAllTimersAsync()
    const out = await p

    expect(out.ok).toBe(true)
    expect(out.stepsRun).toBe(2)
    expect(sendUser).toHaveBeenCalledTimes(2)
    expect(sendUser).toHaveBeenNthCalledWith(1, 's1', '看一下 app.ts')
    expect(sendUser).toHaveBeenNthCalledWith(2, 's1', '修复其中关于 性能 的部分')
  })

  it('stops and reports the failed step if sendUser fails', async () => {
    const { deps, sendUser } = makeDeps()
    sendUser
      .mockResolvedValueOnce({ ok: true })
      .mockResolvedValueOnce({ ok: false, error: 'no session' })
    const skill = makeSkill({
      steps: [
        { type: 'prompt', text: 'first' },
        { type: 'prompt', text: 'second' },
        { type: 'prompt', text: 'third' }
      ]
    })
    const p = runSkill(deps, { sessionId: 's1', skill, vars: {} })
    await vi.runAllTimersAsync()
    const out = await p
    expect(out.ok).toBe(false)
    expect(out.stepsRun).toBe(2)
    expect(out.failedStepIndex).toBe(1)
    expect(out.failedReason).toContain('no session')
    expect(sendUser).toHaveBeenCalledTimes(2)
  })

  it('unsubscribes from cc:data after wait-response completes', async () => {
    const { deps, bus } = makeDeps()
    const skill = makeSkill({
      steps: [
        { type: 'prompt', text: 'hi' },
        { type: 'wait-response', timeoutMs: 600 }
      ]
    })
    const p = runSkill(deps, {
      sessionId: 's1',
      skill,
      vars: {},
      defaultWaitMs: 100
    })
    // Mid-flight: a listener should be active during the wait-response step.
    await vi.advanceTimersByTimeAsync(300)
    bus.fireChunk('s1', 'partial response')
    await vi.runAllTimersAsync()
    await p
    expect(bus.listenerCount()).toBe(0)
  })

  it('touches last_used_at exactly once on a successful run', async () => {
    const { deps, touchLastUsed } = makeDeps()
    const skill = makeSkill({
      steps: [{ type: 'prompt', text: 'hello' }]
    })
    const p = runSkill(deps, { sessionId: 's1', skill, vars: {} })
    await vi.runAllTimersAsync()
    await p
    expect(touchLastUsed).toHaveBeenCalledTimes(1)
    expect(touchLastUsed).toHaveBeenCalledWith(skill.id)
  })

  it('does not touch last_used_at when the run fails', async () => {
    const { deps, sendUser, touchLastUsed } = makeDeps()
    sendUser.mockResolvedValueOnce({ ok: false, error: 'fail' })
    const skill = makeSkill({
      steps: [{ type: 'prompt', text: 'x' }]
    })
    const p = runSkill(deps, { sessionId: 's1', skill, vars: {} })
    await vi.runAllTimersAsync()
    await p
    expect(touchLastUsed).not.toHaveBeenCalled()
  })

  it('returns successfully when there are no steps', async () => {
    const { deps, sendUser } = makeDeps()
    const skill = makeSkill({ steps: [] })
    const p = runSkill(deps, { sessionId: 's1', skill, vars: {} })
    await vi.runAllTimersAsync()
    const out = await p
    expect(out.ok).toBe(true)
    expect(out.stepsRun).toBe(0)
    expect(sendUser).not.toHaveBeenCalled()
  })
})

describe('requiredVariables', () => {
  it('returns the set of unique variables across all prompt steps', () => {
    const skill = makeSkill({
      steps: [
        { type: 'prompt', text: '{a} {b}' },
        { type: 'prompt', text: '{a} {c}' },
        { type: 'wait-response' }
      ]
    })
    expect(requiredVariables(skill).sort()).toEqual(['a', 'b', 'c'])
  })

  it('returns empty for a skill with no placeholders', () => {
    expect(requiredVariables(makeSkill({ steps: [] }))).toEqual([])
  })
})
