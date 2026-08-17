import { describe, expect, it } from 'vitest'
import {
  RemoteImStructuredTaskRegistry
} from '../../../electron/remote-im/structuredTaskRegistry.js'

interface Task {
  taskId: string
  replyId?: string
  value: string
  sourceStarted?: boolean
}

describe('RemoteImStructuredTaskRegistry', () => {
  it('resolves terminal events by task id even without a reply id', () => {
    const registry = new RemoteImStructuredTaskRegistry<Task>()
    const task = { taskId: 'task-1', replyId: 'rim-1', value: 'first' }
    registry.add('session-1', task)

    expect(registry.resolve('session-1', { taskId: 'task-1' })).toBe(task)
  })

  it('falls back to a unique reply id but never guesses between tasks', () => {
    const registry = new RemoteImStructuredTaskRegistry<Task>()
    const first = { taskId: 'task-1', replyId: 'rim-1', value: 'first' }
    const second = { taskId: 'task-2', replyId: 'rim-2', value: 'second' }
    registry.add('session-1', first)

    expect(registry.resolve('session-1', { replyId: 'rim-1' })).toBe(first)
    expect(registry.resolve('session-1', {})).toBe(first)

    registry.add('session-1', second)
    expect(registry.resolve('session-1', {})).toBeUndefined()
    expect(registry.resolve('session-1', { replyId: 'rim-2' })).toBe(second)
  })

  it('does not fall back to the sole route when an explicit identity is wrong', () => {
    const registry = new RemoteImStructuredTaskRegistry<Task>()
    registry.add('session-1', { taskId: 'task-a', replyId: 'rim-a', value: 'A' })

    expect(registry.resolve('session-1', { taskId: 'task-forged' })).toBeUndefined()
    expect(registry.resolve('session-1', { replyId: 'rim-forged' })).toBeUndefined()
    expect(
      registry.resolve('session-1', { taskId: 'task-a', replyId: 'rim-forged' })
    ).toBeUndefined()
    expect(
      registry.resolve('session-1', { taskId: 'task-forged', replyId: 'rim-a' })
    ).toBeUndefined()
  })

  it('removes completed tasks without affecting concurrent tasks', () => {
    const registry = new RemoteImStructuredTaskRegistry<Task>()
    const first = { taskId: 'task-1', replyId: 'rim-1', value: 'first' }
    const second = { taskId: 'task-2', replyId: 'rim-2', value: 'second' }
    registry.add('session-1', first)
    registry.add('session-1', second)

    expect(registry.remove('session-1', 'task-1')).toBe(first)
    expect(registry.list('session-1')).toEqual([second])
  })

  it('enumerates only sessions that still have task authority', () => {
    const registry = new RemoteImStructuredTaskRegistry<Task>()
    registry.add('session-a', { taskId: 'task-a', value: 'A' })
    registry.add('session-b', { taskId: 'task-b', value: 'B' })

    expect(new Set(registry.sessionIds())).toEqual(new Set(['session-a', 'session-b']))
    registry.remove('session-a', 'task-a')
    expect(registry.sessionIds()).toEqual(['session-b'])
  })

  it('retains local-takeover tasks until terminal removal', () => {
    const registry = new RemoteImStructuredTaskRegistry<Task>()
    const task = { taskId: 'task-a', replyId: 'rim-a', value: 'A' }
    registry.add('session-1', task)

    expect(registry.markLocalTakeover('session-1')).toEqual([task])
    expect(registry.isLocallyTakenOver('session-1', task.taskId)).toBe(true)
    expect(registry.list('session-1')).toEqual([task])

    expect(registry.remove('session-1', task.taskId)).toBe(task)
    expect(registry.isLocallyTakenOver('session-1', task.taskId)).toBe(false)
    expect(registry.list('session-1')).toEqual([])
  })

  it('lets an accepted remote steer reclaim a local-takeover route', () => {
    const registry = new RemoteImStructuredTaskRegistry<Task>()
    const task = { taskId: 'task-a', replyId: 'rim-a', value: 'A' }
    registry.add('session-1', task)
    registry.markLocalTakeover('session-1')

    registry.clearLocalTakeover('session-1', task.taskId)

    expect(registry.isLocallyTakenOver('session-1', task.taskId)).toBe(false)
    expect(registry.list('session-1')).toEqual([task])
  })

  it('does not correlate a completed task after the next task starts in the same session', () => {
    const registry = new RemoteImStructuredTaskRegistry<Task>()
    const first = { taskId: 'task-old', replyId: 'rim-old', value: 'old' }
    const second = { taskId: 'task-new', replyId: 'rim-new', value: 'new' }
    registry.add('session-1', first)
    registry.remove('session-1', first.taskId)
    registry.add('session-1', second)

    expect(registry.resolve('session-1', { taskId: first.taskId })).toBeUndefined()
    expect(registry.resolve('session-1', { replyId: first.replyId })).toBeUndefined()
    expect(registry.resolve('session-1', { taskId: second.taskId })).toBe(second)
  })

  it('removes superseded tasks when a newer task starts in the same session', () => {
    const registry = new RemoteImStructuredTaskRegistry<Task>()
    const first = { taskId: 'task-1', replyId: 'rim-1', value: 'first' }
    const second = { taskId: 'task-2', replyId: 'rim-2', value: 'second' }
    registry.add('session-1', first)
    registry.add('session-1', second)

    expect(registry.removeAllExcept('session-1', 'task-2')).toEqual([first])
    expect(registry.list('session-1')).toEqual([second])
  })

  it('keeps a newer pending task when an older registered task starts first', () => {
    const registry = new RemoteImStructuredTaskRegistry<Task>()
    const first = {
      taskId: 'task-1',
      replyId: 'rim-1',
      value: 'first',
      sourceStarted: true
    }
    const second = {
      taskId: 'task-2',
      replyId: 'rim-2',
      value: 'second',
      sourceStarted: false
    }
    registry.add('session-1', first)
    registry.add('session-1', second)

    expect(
      registry.removeAllExcept('session-1', 'task-1', (task) => task.sourceStarted === true)
    ).toEqual([])
    expect(registry.list('session-1')).toEqual([first, second])
  })
})
