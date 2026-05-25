/**
 * KB scheduler: decides when (and when NOT) to run a project-knowledge-base
 * summarization pass. All decision logic is a pure function — the wrapper
 * that runs every 60s lives below it and is the only piece that touches
 * the database or the file system.
 */

export interface KbSchedulerSignals {
  /** Epoch ms of the most recent successful summary for this repo. 0 = never. */
  lastSummaryAt: number
  /** Epoch ms of the most recent `cc:data` chunk on the main session. */
  lastAiActivityAt: number
  /** Epoch ms of the most recent user-submitted prompt on the main session. */
  lastUserPromptAt: number
  /** Count of new ai_prompt_main events since lastSummaryAt. */
  pendingSignalCount: number
  /** True when the user has the main session in 'running' state. */
  mainSessionRunning: boolean
  /** True when the renderer hasn't reported a CLI is configured yet. */
  cliConfigured: boolean
}

export type KbRunVerdict =
  | { run: true; reason: 'idle-window' | 'signal-threshold' | 'long-overdue' }
  | { run: false; reason: KbSkipReason }

export type KbSkipReason =
  | 'main-session-not-running'
  | 'no-cli-config'
  | 'ai-streaming'
  | 'user-just-prompted'
  | 'rate-limited'
  | 'no-new-signal'
  | 'thresholds-not-met'

export interface KbSchedulerThresholds {
  /** User considered idle when no prompt for this long. */
  idleWindowMs: number
  /** Pending signals at or above this number trigger an opportunistic run. */
  signalThreshold: number
  /** Hard ceiling — even with very little activity, run at least this often. */
  longOverdueMs: number
  /** Skip if main session got a data chunk this recently (AI is streaming). */
  aiStreamingWindowMs: number
  /** Skip if user just submitted (give them time to read AI reply). */
  userPromptCooldownMs: number
  /** Skip if we already summarized within this window (rate limit). */
  summaryCooldownMs: number
}

/** Default thresholds — exported for tests / UI display. */
export const KB_SCHEDULER_DEFAULTS: KbSchedulerThresholds = {
  idleWindowMs: 15 * 60 * 1000,
  signalThreshold: 5,
  longOverdueMs: 24 * 60 * 60 * 1000,
  aiStreamingWindowMs: 10 * 1000,
  userPromptCooldownMs: 30 * 1000,
  summaryCooldownMs: 30 * 60 * 1000
}

/**
 * Pure decision function: given current signals + clock + thresholds,
 * return whether we should run a summary pass right now.
 *
 * Order of checks matters: hard-skips come first so they short-circuit
 * the more permissive run conditions.
 */
export function decideKbRun(
  signals: KbSchedulerSignals,
  now: number,
  thresholds: KbSchedulerThresholds = KB_SCHEDULER_DEFAULTS
): KbRunVerdict {
  if (!signals.mainSessionRunning) {
    return { run: false, reason: 'main-session-not-running' }
  }
  if (!signals.cliConfigured) {
    return { run: false, reason: 'no-cli-config' }
  }
  // Streaming check — never interrupt the AI mid-stream.
  if (
    signals.lastAiActivityAt > 0 &&
    now - signals.lastAiActivityAt < thresholds.aiStreamingWindowMs
  ) {
    return { run: false, reason: 'ai-streaming' }
  }
  // User just hit send — give them time to read the reply before we fire
  // a parallel CLI subprocess.
  if (
    signals.lastUserPromptAt > 0 &&
    now - signals.lastUserPromptAt < thresholds.userPromptCooldownMs
  ) {
    return { run: false, reason: 'user-just-prompted' }
  }
  // Rate limit on the summary itself.
  if (
    signals.lastSummaryAt > 0 &&
    now - signals.lastSummaryAt < thresholds.summaryCooldownMs
  ) {
    return { run: false, reason: 'rate-limited' }
  }
  // Nothing has happened since last run → no work to do.
  if (signals.pendingSignalCount === 0) {
    return { run: false, reason: 'no-new-signal' }
  }

  // Now decide which positive trigger fires. Higher-confidence first.
  const sinceLastSummary = signals.lastSummaryAt === 0
    ? Infinity
    : now - signals.lastSummaryAt
  if (
    sinceLastSummary >= thresholds.longOverdueMs &&
    signals.pendingSignalCount >= 1
  ) {
    return { run: true, reason: 'long-overdue' }
  }
  if (signals.pendingSignalCount >= thresholds.signalThreshold) {
    return { run: true, reason: 'signal-threshold' }
  }
  // Idle-window trigger — user has been quiet long enough to do work.
  const sinceLastPrompt = signals.lastUserPromptAt === 0
    ? Infinity
    : now - signals.lastUserPromptAt
  if (sinceLastPrompt >= thresholds.idleWindowMs) {
    return { run: true, reason: 'idle-window' }
  }
  return { run: false, reason: 'thresholds-not-met' }
}

/**
 * Returns a stable human-readable message for a skip reason. Used by the
 * UI's "next-tick reason" display and by tests.
 */
export function describeSkipReason(reason: KbSkipReason): string {
  switch (reason) {
    case 'main-session-not-running':
      return '主会话未在运行'
    case 'no-cli-config':
      return '未配置 CLI'
    case 'ai-streaming':
      return 'AI 正在流式输出，等它结束'
    case 'user-just-prompted':
      return '你刚提交过提示，让 AI 先答完'
    case 'rate-limited':
      return '距上次总结不足 30 分钟'
    case 'no-new-signal':
      return '没有新的可总结的提示'
    case 'thresholds-not-met':
      return '等更多空闲时间或更多新提示'
  }
}
