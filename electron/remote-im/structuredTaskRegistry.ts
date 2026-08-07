export interface RemoteImStructuredTaskIdentity {
  taskId: string
  replyId?: string
}

export class RemoteImStructuredTaskRegistry<T extends RemoteImStructuredTaskIdentity> {
  private readonly tasksBySession = new Map<string, Map<string, T>>()

  add(sessionId: string, task: T): void {
    let tasks = this.tasksBySession.get(sessionId)
    if (!tasks) {
      tasks = new Map()
      this.tasksBySession.set(sessionId, tasks)
    }
    tasks.set(task.taskId, task)
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
      if (task) return task
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
      removed.push(task)
    }
    if (tasks.size === 0) this.tasksBySession.delete(sessionId)
    return removed
  }

  list(sessionId: string): T[] {
    return [...(this.tasksBySession.get(sessionId)?.values() ?? [])]
  }
}
