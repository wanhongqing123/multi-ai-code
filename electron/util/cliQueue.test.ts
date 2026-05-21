import { beforeEach, describe, expect, it } from 'vitest'
import { _resetCliQueueForTests, enqueueCliJob, getCliQueueState } from './cliQueue.js'

beforeEach(() => {
  _resetCliQueueForTests()
})

describe('cliQueue', () => {
  it('runs jobs serially in FIFO order', async () => {
    const log: string[] = []
    const settle: Array<() => void> = []
    const inFlight: Promise<unknown>[] = []
    for (const label of ['a', 'b', 'c']) {
      inFlight.push(
        enqueueCliJob(label, async () => {
          log.push(`start:${label}`)
          await new Promise<void>((r) => settle.push(r))
          log.push(`end:${label}`)
        })
      )
    }
    // Wait for the first job to actually start (queue grabs it via .then).
    await Promise.resolve()
    await Promise.resolve()
    expect(log).toEqual(['start:a'])
    settle[0]()
    await new Promise((r) => setImmediate(r))
    expect(log).toEqual(['start:a', 'end:a', 'start:b'])
    settle[1]()
    await new Promise((r) => setImmediate(r))
    expect(log).toEqual([
      'start:a',
      'end:a',
      'start:b',
      'end:b',
      'start:c'
    ])
    settle[2]()
    await Promise.all(inFlight)
    expect(log).toEqual([
      'start:a',
      'end:a',
      'start:b',
      'end:b',
      'start:c',
      'end:c'
    ])
  })

  it('returns the job result to the caller', async () => {
    const result = await enqueueCliJob('one', async () => 42)
    expect(result).toBe(42)
  })

  it('isolates errors so a failed job does not stop the queue', async () => {
    const ok1 = enqueueCliJob('first', async () => 'first')
    const bad = enqueueCliJob('boom', async () => {
      throw new Error('nope')
    })
    const ok2 = enqueueCliJob('third', async () => 'third')
    expect(await ok1).toBe('first')
    await expect(bad).rejects.toThrow('nope')
    expect(await ok2).toBe('third')
  })

  it('reports queue state (running + waiting) accurately', async () => {
    let releaseA: () => void = () => {}
    enqueueCliJob('A', () => new Promise<void>((r) => (releaseA = r)))
    enqueueCliJob('B', async () => {})
    enqueueCliJob('C', async () => {})

    // Let microtasks run so 'A' enters active state.
    await Promise.resolve()
    await Promise.resolve()

    const state = getCliQueueState()
    expect(state.running?.label).toBe('A')
    expect(state.waiting.map((j) => j.label)).toEqual(['B', 'C'])

    releaseA()
    await new Promise((r) => setImmediate(r))
    await new Promise((r) => setImmediate(r))
    const after = getCliQueueState()
    expect(after.waiting.map((j) => j.label)).toEqual([])
  })
})
