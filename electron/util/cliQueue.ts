/**
 * Process-wide serial queue for background CLI subprocess jobs.
 *
 * Both the habit-learning generator and the project-KB summarizer spawn the
 * user's main AI CLI in one-shot (`-p` / `exec`) mode. If they fire at the
 * same time they compete for the same OAuth refresh tokens and run into
 * rate limits, so we funnel both through a single FIFO here.
 *
 * The queue is intentionally simple: an internal promise chain, one job at
 * a time. Failures are isolated per-job (the chain's `.catch` swallows
 * rejections so a single bad job can't poison subsequent ones).
 */

interface CliJob {
  label: string
  enqueuedAt: number
}

let tail: Promise<unknown> = Promise.resolve()
const activeJobs: CliJob[] = []
const waitingJobs: CliJob[] = []

export interface CliQueueState {
  /** Currently-running job, or null if the queue is idle. */
  running: CliJob | null
  /** Jobs enqueued but not yet started. */
  waiting: CliJob[]
}

export function getCliQueueState(): CliQueueState {
  return {
    running: activeJobs[0] ?? null,
    waiting: waitingJobs.slice()
  }
}

/**
 * Schedule `fn` to run after every previously enqueued job has settled.
 * The returned promise resolves with `fn`'s value or rejects with its
 * error; either way, the next queued job proceeds.
 */
export function enqueueCliJob<T>(
  label: string,
  fn: () => Promise<T>
): Promise<T> {
  const job: CliJob = { label, enqueuedAt: Date.now() }
  waitingJobs.push(job)

  const result = tail.then(async () => {
    // Move from waiting → active. Use shift so labels render in FIFO order.
    const idx = waitingJobs.indexOf(job)
    if (idx >= 0) waitingJobs.splice(idx, 1)
    activeJobs.push(job)
    try {
      return await fn()
    } finally {
      const aIdx = activeJobs.indexOf(job)
      if (aIdx >= 0) activeJobs.splice(aIdx, 1)
    }
  })

  // Keep the chain alive even when `fn` throws — without this, one error
  // would unhandle-reject the tail and stop the queue from accepting more.
  tail = result.catch(() => undefined)
  return result
}

/** Reset the queue. For tests only — do not call from production code. */
export function _resetCliQueueForTests(): void {
  tail = Promise.resolve()
  activeJobs.length = 0
  waitingJobs.length = 0
}
