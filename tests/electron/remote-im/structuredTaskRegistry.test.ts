import { describe, expect, it } from 'vitest'
import { RemoteImStructuredTaskRegistry } from '../../../electron/remote-im/structuredTaskRegistry.js'

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

  it('removes completed tasks without affecting concurrent tasks', () => {
    const registry = new RemoteImStructuredTaskRegistry<Task>()
    const first = { taskId: 'task-1', replyId: 'rim-1', value: 'first' }
    const second = { taskId: 'task-2', replyId: 'rim-2', value: 'second' }
    registry.add('session-1', first)
    registry.add('session-1', second)

    expect(registry.remove('session-1', 'task-1')).toBe(first)
    expect(registry.list('session-1')).toEqual([second])
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
