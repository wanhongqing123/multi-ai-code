import { describe, expect, it } from 'vitest'
import {
  evaluateRemoteImStructuredTaskAdmission,
  RemoteImQueuedInputRegistry,
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

  it('keeps a local-takeover tombstone closed to another remote authority', () => {
    type AuthorizedTask = Task & { projectId: string; toUserId: string }
    const registry = new RemoteImStructuredTaskRegistry<AuthorizedTask>()
    const task: AuthorizedTask = {
      taskId: 'task-a',
      replyId: 'rim-a',
      value: 'A',
      projectId: 'project-a',
      toUserId: 'phone-a'
    }
    registry.add('session-1', task)
    registry.markLocalTakeover('session-1')

    expect(
      evaluateRemoteImStructuredTaskAdmission(registry.list('session-1'), {
        projectId: 'project-a',
        toUserId: 'phone-b'
      })
    ).toEqual({ ok: false, reason: 'authority-conflict' })
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

describe('evaluateRemoteImStructuredTaskAdmission', () => {
  const active = {
    projectId: 'project-a',
    toUserId: 'phone-a'
  }

  it('allows the first task but blocks normal same-requester steer while it is active', () => {
    expect(
      evaluateRemoteImStructuredTaskAdmission([], active)
    ).toEqual({ ok: true })
    expect(
      evaluateRemoteImStructuredTaskAdmission([active], active)
    ).toEqual({ ok: false, reason: 'active-task' })
  })

  it('never shares one task authority across requesters or projects', () => {
    expect(
      evaluateRemoteImStructuredTaskAdmission([active], {
        projectId: 'project-a',
        toUserId: 'phone-b'
      })
    ).toEqual({ ok: false, reason: 'authority-conflict' })
    expect(
      evaluateRemoteImStructuredTaskAdmission([active], {
        projectId: 'project-b',
        toUserId: 'phone-a'
      })
    ).toEqual({ ok: false, reason: 'authority-conflict' })
  })
})

describe('RemoteImQueuedInputRegistry', () => {
  it('dequeues machine inputs once in FIFO order and isolates sessions', () => {
    const queue = new RemoteImQueuedInputRegistry<string>(32, 5 * 60 * 1000)
    expect(queue.enqueue('session-a', 'first', 100)).toMatchObject({ ok: true })
    expect(queue.enqueue('session-a', 'second', 101)).toMatchObject({ ok: true })
    expect(queue.enqueue('session-b', 'other', 102)).toMatchObject({ ok: true })

    expect(queue.take('session-a', 103).entry?.value).toBe('first')
    expect(queue.take('session-a', 104).entry?.value).toBe('second')
    expect(queue.take('session-a', 105).entry).toBeUndefined()
    expect(queue.take('session-b', 106).entry?.value).toBe('other')
  })

  it('bounds each session independently and admits new work after a terminal dequeue', () => {
    const queue = new RemoteImQueuedInputRegistry<string>(2, 1000)
    expect(queue.enqueue('session-a', 'first', 0)).toMatchObject({ ok: true })
    expect(queue.enqueue('session-a', 'second', 1)).toMatchObject({ ok: true })
    expect(queue.enqueue('session-a', 'overflow', 2)).toEqual({
      ok: false,
      reason: 'capacity',
      expired: []
    })
    expect(queue.enqueue('session-b', 'independent', 2)).toMatchObject({ ok: true })

    expect(queue.take('session-a', 3).entry?.value).toBe('first')
    expect(queue.enqueue('session-a', 'third', 4)).toMatchObject({ ok: true })
    expect(queue.take('session-a', 5).entry?.value).toBe('second')
    expect(queue.take('session-a', 6).entry?.value).toBe('third')
  })

  it('drops expired closures instead of submitting stale machine input', () => {
    const queue = new RemoteImQueuedInputRegistry<string>(2, 100)
    queue.enqueue('session-a', 'expired', 10)

    const result = queue.take('session-a', 110)

    expect(result.entry).toBeUndefined()
    expect(result.expired).toEqual([{ value: 'expired', queuedAt: 10 }])
    expect(queue.size('session-a')).toBe(0)
  })

  it('removes queued inputs by account, contact, or session predicate', () => {
    type Queued = { sessionId: string; userId: string }
    const queue = new RemoteImQueuedInputRegistry<Queued>(32, 1000)
    queue.enqueue('session-a', { sessionId: 'session-a', userId: 'phone-a' }, 1)
    queue.enqueue('session-a', { sessionId: 'session-a', userId: 'phone-b' }, 2)
    queue.enqueue('session-b', { sessionId: 'session-b', userId: 'phone-a' }, 3)

    expect(
      queue.removeWhere((item) => item.userId === 'phone-a').map((entry) => entry.value)
    ).toEqual([
      { sessionId: 'session-a', userId: 'phone-a' },
      { sessionId: 'session-b', userId: 'phone-a' }
    ])
    expect(queue.size('session-a')).toBe(1)
    expect(queue.size('session-b')).toBe(0)
    expect(
      queue.removeWhere((item) => item.sessionId === 'session-a').map((entry) => entry.value)
    ).toEqual([{ sessionId: 'session-a', userId: 'phone-b' }])
  })
})
