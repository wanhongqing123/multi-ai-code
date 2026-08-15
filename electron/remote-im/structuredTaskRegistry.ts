export interface RemoteImStructuredTaskIdentity {
  taskId: string
  replyId?: string
}

export interface RemoteImStructuredTaskAuthority {
  projectId: string
  toUserId: string
}

export type RemoteImStructuredTaskAdmission =
  | { ok: true }
  | { ok: false; reason: 'active-task' | 'authority-conflict' }

/**
 * Decide synchronously, before submitting another message to a source-level
 * AICLI session, whether its output/approval authority is unambiguous.
 *
 * A normal follow-up can become a steer inside the current Codex turn, which
 * has no new TurnStarted identity. We therefore fail closed while any remote
 * task is active. Parallel source-level routes remain fail-closed until every
 * task type carries immutable thread/turn/task authority end to end.
 */
export function evaluateRemoteImStructuredTaskAdmission<
  T extends RemoteImStructuredTaskAuthority
>(
  existing: readonly T[],
  candidate: RemoteImStructuredTaskAuthority
): RemoteImStructuredTaskAdmission {
  if (existing.length === 0) return { ok: true }
  const hasAuthorityConflict = existing.some(
    (task) =>
      task.projectId !== candidate.projectId || task.toUserId !== candidate.toUserId
  )
  if (hasAuthorityConflict) return { ok: false, reason: 'authority-conflict' }
  return { ok: false, reason: 'active-task' }
}

export class RemoteImStructuredTaskRegistry<T extends RemoteImStructuredTaskIdentity> {
  private readonly tasksBySession = new Map<string, Map<string, T>>()
  private readonly locallyTakenOverTaskIdsBySession = new Map<string, Set<string>>()

  add(sessionId: string, task: T): void {
    let tasks = this.tasksBySession.get(sessionId)
    if (!tasks) {
      tasks = new Map()
      this.tasksBySession.set(sessionId, tasks)
    }
    tasks.set(task.taskId, task)
    this.locallyTakenOverTaskIdsBySession.get(sessionId)?.delete(task.taskId)
  }

  /**
   * Stop forwarding a remote task after local TUI input without releasing its
   * admission lock. A local steer can remain inside the same Codex turn, so the
   * task must block another remote authority until a real terminal event (or
   * process exit) removes it.
   */
  markLocalTakeover(sessionId: string): T[] {
    const tasks = this.tasksBySession.get(sessionId)
    if (!tasks?.size) return []
    let taskIds = this.locallyTakenOverTaskIdsBySession.get(sessionId)
    if (!taskIds) {
      taskIds = new Set()
      this.locallyTakenOverTaskIdsBySession.set(sessionId, taskIds)
    }
    for (const taskId of tasks.keys()) taskIds.add(taskId)
    return [...tasks.values()]
  }

  isLocallyTakenOver(sessionId: string, taskId: string): boolean {
    return this.locallyTakenOverTaskIdsBySession.get(sessionId)?.has(taskId) === true
  }

  resolve(
    sessionId: string,
    identity: { taskId?: string; replyId?: string },
    options: { allowSoleFallback?: boolean } = {}
  ): T | undefined {
    const tasks = this.tasksBySession.get(sessionId)
    if (!tasks?.size) return undefined

    if (identity.taskId) {
      const task = tasks.get(identity.taskId)
      if (!task) return undefined
      if (identity.replyId && task.replyId !== identity.replyId) return undefined
      return task
    }

    if (identity.replyId) {
      const matches = [...tasks.values()].filter((task) => task.replyId === identity.replyId)
      if (matches.length === 1) return matches[0]
      return undefined
    }

    // Markerless terminal events are safe only when the source session has a
    // single active remote task. Never guess between concurrent requests.
    if (
      options.allowSoleFallback !== false &&
      !identity.taskId &&
      !identity.replyId &&
      tasks.size === 1
    ) {
      return tasks.values().next().value
    }
    return undefined
  }

  remove(sessionId: string, taskId: string): T | undefined {
    const tasks = this.tasksBySession.get(sessionId)
    if (!tasks) return undefined
    const task = tasks.get(taskId)
    if (!task) return undefined
    tasks.delete(taskId)
    const locallyTakenOverTaskIds = this.locallyTakenOverTaskIdsBySession.get(sessionId)
    locallyTakenOverTaskIds?.delete(taskId)
    if (locallyTakenOverTaskIds?.size === 0) {
      this.locallyTakenOverTaskIdsBySession.delete(sessionId)
    }
    if (tasks.size === 0) this.tasksBySession.delete(sessionId)
    return task
  }

  removeAllExcept(
    sessionId: string,
    taskId: string,
    shouldRemove: (task: T) => boolean = () => true
  ): T[] {
    const tasks = this.tasksBySession.get(sessionId)
    if (!tasks) return []

    const removed: T[] = []
    for (const [candidateTaskId, task] of tasks) {
      if (candidateTaskId === taskId || !shouldRemove(task)) continue
      tasks.delete(candidateTaskId)
      this.locallyTakenOverTaskIdsBySession.get(sessionId)?.delete(candidateTaskId)
      removed.push(task)
    }
    if (this.locallyTakenOverTaskIdsBySession.get(sessionId)?.size === 0) {
      this.locallyTakenOverTaskIdsBySession.delete(sessionId)
    }
    if (tasks.size === 0) this.tasksBySession.delete(sessionId)
    return removed
  }

  list(sessionId: string): T[] {
    return [...(this.tasksBySession.get(sessionId)?.values() ?? [])]
  }

  sessionIds(): string[] {
    return [...this.tasksBySession.keys()]
  }
}

export interface RemoteImQueuedInputEntry<T> {
  value: T
  queuedAt: number
}

export type RemoteImQueuedInputEnqueueResult<T> =
  | { ok: true; expired: RemoteImQueuedInputEntry<T>[] }
  | { ok: false; reason: 'capacity'; expired: RemoteImQueuedInputEntry<T>[] }

/**
 * Small bounded FIFO used for machine-to-machine inputs that arrive while the
 * previous AICLI turn still owns output correlation. Queue policy lives in a
 * side-effect-free type so ordering, capacity, and expiry remain testable
 * without importing Electron's main-process IPC module.
 */
export class RemoteImQueuedInputRegistry<T> {
  private readonly entriesBySession = new Map<string, RemoteImQueuedInputEntry<T>[]>()

  constructor(
    private readonly maxEntriesPerSession: number,
    private readonly ttlMs: number
  ) {
    if (!Number.isInteger(maxEntriesPerSession) || maxEntriesPerSession < 1) {
      throw new Error('maxEntriesPerSession must be a positive integer')
    }
    if (!Number.isFinite(ttlMs) || ttlMs < 1) {
      throw new Error('ttlMs must be positive')
    }
  }

  enqueue(
    sessionId: string,
    value: T,
    now = Date.now()
  ): RemoteImQueuedInputEnqueueResult<T> {
    const expired = this.pruneExpired(sessionId, now)
    const entries = this.entriesBySession.get(sessionId) ?? []
    if (entries.length >= this.maxEntriesPerSession) {
      return { ok: false, reason: 'capacity', expired }
    }
    entries.push({ value, queuedAt: now })
    this.entriesBySession.set(sessionId, entries)
    return { ok: true, expired }
  }

  take(
    sessionId: string,
    now = Date.now()
  ): {
    entry?: RemoteImQueuedInputEntry<T>
    expired: RemoteImQueuedInputEntry<T>[]
  } {
    const expired = this.pruneExpired(sessionId, now)
    const entries = this.entriesBySession.get(sessionId)
    const entry = entries?.shift()
    if (!entries?.length) this.entriesBySession.delete(sessionId)
    return { ...(entry ? { entry } : {}), expired }
  }

  removeWhere(predicate: (value: T) => boolean): RemoteImQueuedInputEntry<T>[] {
    const removed: RemoteImQueuedInputEntry<T>[] = []
    for (const [sessionId, entries] of this.entriesBySession) {
      const retained: RemoteImQueuedInputEntry<T>[] = []
      for (const entry of entries) {
        if (predicate(entry.value)) removed.push(entry)
        else retained.push(entry)
      }
      if (retained.length > 0) this.entriesBySession.set(sessionId, retained)
      else this.entriesBySession.delete(sessionId)
    }
    return removed
  }

  expire(sessionId: string, now = Date.now()): RemoteImQueuedInputEntry<T>[] {
    return this.pruneExpired(sessionId, now)
  }

  nextExpiryAt(sessionId: string): number | undefined {
    const queuedAt = this.entriesBySession.get(sessionId)?.[0]?.queuedAt
    return queuedAt === undefined ? undefined : queuedAt + this.ttlMs
  }

  size(sessionId: string): number {
    return this.entriesBySession.get(sessionId)?.length ?? 0
  }

  private pruneExpired(sessionId: string, now: number): RemoteImQueuedInputEntry<T>[] {
    const entries = this.entriesBySession.get(sessionId)
    if (!entries?.length) return []
    const expired: RemoteImQueuedInputEntry<T>[] = []
    const retained: RemoteImQueuedInputEntry<T>[] = []
    for (const entry of entries) {
      if (now - entry.queuedAt >= this.ttlMs) expired.push(entry)
      else retained.push(entry)
    }
    if (retained.length > 0) this.entriesBySession.set(sessionId, retained)
    else this.entriesBySession.delete(sessionId)
    return expired
  }
}
